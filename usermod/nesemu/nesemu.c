// nesemu - MicroPython user C module: small NES emulator (own core, see nes_core.h). No sound.
//
//   import nesemu
//   emu = nesemu.Emu(open("game.nes", "rb"), slots, strips)   # ROM stays on the SD card
//   emu.sd_direct(spi_id, cs_pin, cdv)                        # fast ROM reads straight from the card
//   emu.display_hw(spi_id, dc, cs, x, y, mode, baud)          # DMA frames to the ST7789 (256x240)
//   emu.options(frame_skip)                                   # draw only every other frame
//   emu.frame(buttons, draw=True)                             # run one frame
//   emu.sync() / emu.stats() / emu.info() / emu.ram() / emu.dirty() / emu.title() / emu.mapper()
//
// ROM data is cached in 4 KB units (PRG units first, then CHR units). The 4 KB PRG windows and
// 1 KB CHR windows of the core point straight into cache slots; slots that a window uses are
// pinned, the rest is replaced least-recently-used.

#include <string.h>

#include "py/runtime.h"
#include "py/obj.h"
#include "py/objarray.h"
#include "py/stream.h"
#include "py/mperrno.h"
#include "py/mphal.h"

#include "extmod/vfs_fat.h"
#include "lib/oofatfs/ff.h"

#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "py/gc.h"
#include "hardware/sync.h"

#define NES_HOT __attribute__((section(".time_critical.nesemu")))
#define NES_TBL static __attribute__((section(".time_critical.nesemu_t")))   // opcode tables in RAM
#include "nes_core.h"

#define NE_UNIT 4096
#define NE_USHIFT 12
#define NE_SLOTB (NE_UNIT + 512)    // slot block: data at +16 so a 9-sector read lands aligned
#define NE_DOFF 16
#define NE_MAXS 120
#define NE_STRIP 8                  // lines per display strip
#define NE_RMAX 16
#define NE_W 256
#define NE_MINSLOTS 20

// button bits expected from Python (same as buttons.py)
#define BT_UP 1
#define BT_DOWN 2
#define BT_LEFT 4
#define BT_RIGHT 8
#define BT_A 16
#define BT_B 32
#define BT_START 64
#define BT_SELECT 128

typedef struct _ne_obj_t {
    mp_obj_base_t base;
    nes_t *nes;
    mp_obj_t file;
    const mp_stream_p_t *stream;
    uint32_t file_size, hdr_bytes, nunits_prg, nunits;
    uint8_t **slot_buf;
    int nslots;
    int32_t *slot_unit;
    uint32_t *slot_use, *slot_pin;
    uint16_t *utab;           // unit -> slot + 1
    uint32_t tick, epoch;
    uint32_t win_off[16];     // offset each window was last resolved for (0xFFFFFFFF = none)
    int16_t win_slot[16];
    bool frame_skip, skip_phase;
    bool draw_on;
    uint16_t lut[64];
    int ox, oy;
    int pending, pend_y;
    bool hw;
    volatile bool dma_busy;
    uint16_t *ring_buf[NE_RMAX];
    int ring_n;
    int s_nl[NE_RMAX], s_y0[NE_RMAX];
    volatile uint32_t q_head, q_tail;
    spi_inst_t *spi;
    unsigned dc, cs;
    bool sd_ok;
    spi_inst_t *sd_spi;
    unsigned sd_cs;
    uint32_t sd_cdv, sd_cl_bytes;
    uint32_t *sd_table;
    uint32_t st_loads, st_load_us, st_wait_us, st_drawn, st_strips;
} ne_obj_t;

extern const mp_obj_type_t ne_type;
static int ne_dma = -1;
static ne_obj_t *ne_active = NULL;
static bool ne_irq_ready = false;

// NES master palette (2C02), RGB
static const uint8_t ne_rgb[64][3] = {
    {84, 84, 84}, {0, 30, 116}, {8, 16, 144}, {48, 0, 136}, {68, 0, 100}, {92, 0, 48}, {84, 4, 0}, {60, 24, 0},
    {32, 42, 0}, {8, 58, 0}, {0, 64, 0}, {0, 60, 0}, {0, 50, 60}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
    {152, 150, 152}, {8, 76, 196}, {48, 50, 236}, {92, 30, 228}, {136, 20, 176}, {160, 20, 100}, {152, 34, 32}, {120, 60, 0},
    {84, 90, 0}, {40, 114, 0}, {8, 124, 0}, {0, 118, 40}, {0, 102, 120}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
    {236, 238, 236}, {76, 154, 236}, {120, 124, 236}, {176, 98, 236}, {228, 84, 236}, {236, 88, 180}, {236, 106, 100}, {212, 136, 32},
    {160, 170, 0}, {116, 196, 0}, {76, 208, 32}, {56, 204, 108}, {56, 180, 204}, {60, 60, 60}, {0, 0, 0}, {0, 0, 0},
    {236, 238, 236}, {168, 204, 236}, {188, 188, 236}, {212, 178, 236}, {236, 174, 236}, {236, 174, 212}, {236, 180, 176}, {228, 196, 144},
    {204, 210, 120}, {180, 222, 120}, {168, 226, 144}, {152, 226, 180}, {160, 214, 228}, {160, 162, 160}, {0, 0, 0}, {0, 0, 0},
};

// ------------------------------------------------------------------ file access

static void ne_read_at(ne_obj_t *self, uint32_t offset, uint8_t *dst, uint32_t len) {
    int errcode = 0;
    struct mp_stream_seek_t seek = {.offset = (mp_off_t)offset, .whence = MP_SEEK_SET};
    if (self->stream->ioctl(self->file, MP_STREAM_SEEK, (uintptr_t)&seek, &errcode) == MP_STREAM_ERROR) {
        mp_raise_OSError(errcode);
    }
    uint32_t got = 0;
    while (got < len) {
        mp_uint_t n = self->stream->read(self->file, dst + got, len - got, &errcode);
        if (n == MP_STREAM_ERROR) {
            mp_raise_OSError(errcode);
        }
        if (n == 0) {
            break;
        }
        got += n;
    }
    if (got < len) {
        memset(dst + got, 0xFF, len - got);
    }
}

// ------------------------------------------------------------------ direct SD reading

typedef struct _ne_fat_file_t {   // same layout as pyb_file_obj_t in extmod/vfs_fat_file.c
    mp_obj_base_t base;
    FIL fp;
} ne_fat_file_t;

static bool ne_sd_cmd12(ne_obj_t *self) {
    uint8_t c[6] = {0x4C, 0, 0, 0, 0, 0xFF};
    uint8_t b = 0xFF;
    spi_write_blocking(self->sd_spi, c, 6);
    spi_read_blocking(self->sd_spi, 0xFF, &b, 1);
    for (int i = 0; i < 16; i++) {
        spi_read_blocking(self->sd_spi, 0xFF, &b, 1);
        if (!(b & 0x80)) {
            break;
        }
    }
    uint32_t t0 = mp_hal_ticks_us();
    do {
        spi_read_blocking(self->sd_spi, 0xFF, &b, 1);
    } while (b != 0xFF && (mp_hal_ticks_us() - t0) < 200000);
    return b == 0xFF;
}

static bool ne_sd_read(ne_obj_t *self, uint32_t sector, uint8_t *dst, uint32_t nsec) {
    spi_inst_t *spi = self->sd_spi;
    uint32_t arg = sector * self->sd_cdv;
    uint8_t c[6] = {0x40 | 18, arg >> 24, arg >> 16, arg >> 8, arg, 0xFF};
    uint8_t b = 0xFF;
    gpio_put(self->sd_cs, 0);
    spi_write_blocking(spi, c, 6);
    for (int i = 0; i < 16; i++) {
        spi_read_blocking(spi, 0xFF, &b, 1);
        if (!(b & 0x80)) {
            break;
        }
    }
    bool ok = (b == 0);
    for (uint32_t i = 0; ok && i < nsec; i++) {
        uint32_t t0 = mp_hal_ticks_us();
        do {
            spi_read_blocking(spi, 0xFF, &b, 1);
        } while (b == 0xFF && (mp_hal_ticks_us() - t0) < 100000);
        if (b != 0xFE) {
            ok = false;
            break;
        }
        spi_read_blocking(spi, 0xFF, dst + i * 512, 512);
        uint8_t crc[2];
        spi_read_blocking(spi, 0xFF, crc, 2);
    }
    if (!ne_sd_cmd12(self)) {
        ok = false;
    }
    gpio_put(self->sd_cs, 1);
    uint8_t ff = 0xFF;
    spi_write_blocking(spi, &ff, 1);
    return ok;
}

// card sector holding file sector number `fs`
static inline uint32_t ne_sec_of(ne_obj_t *self, uint32_t fs) {
    uint32_t byte = fs << 9;
    return self->sd_table[byte / self->sd_cl_bytes] + ((byte % self->sd_cl_bytes) >> 9);
}

// read unit `u` into a slot block (data lands at block + 16); false -> use the normal path
static bool ne_sd_unit(ne_obj_t *self, uint32_t u, uint8_t *blk) {
    uint32_t s0 = (self->hdr_bytes >> 9) + u * 8;
    uint32_t total = (self->file_size + 511) >> 9;
    if (s0 >= total) {
        return false;
    }
    uint32_t n = total - s0;
    if (n > 9) {
        n = 9;
    }
    uint32_t i = 0;
    while (i < n) {
        uint32_t run = 1;
        uint32_t base = ne_sec_of(self, s0 + i);
        while (i + run < n && ne_sec_of(self, s0 + i + run) == base + run) {
            run++;
        }
        if (!ne_sd_read(self, base, blk + i * 512, run)) {
            return false;
        }
        i += run;
    }
    uint32_t off = u << NE_USHIFT;
    uint32_t dlen = (self->nunits * (uint32_t)NE_UNIT);
    (void)dlen;
    uint32_t avail = self->file_size - self->hdr_bytes;
    if (off + NE_UNIT > avail) {
        uint32_t have = off < avail ? avail - off : 0;
        memset(blk + NE_DOFF + have, 0xFF, NE_UNIT - have);
    }
    return true;
}

// ------------------------------------------------------------------ cache

static uint8_t *ne_load_unit(ne_obj_t *self, uint32_t u) {
    int slot = (int)self->utab[u] - 1;
    if (slot < 0) {
        uint32_t oldest = 0xFFFFFFFFu;
        slot = -1;
        for (int i = 0; i < self->nslots; i++) {
            if (self->slot_pin[i] == self->epoch) {
                continue;
            }
            if (self->slot_unit[i] < 0) {
                slot = i;
                break;
            }
            if (self->slot_use[i] < oldest) {
                oldest = self->slot_use[i];
                slot = i;
            }
        }
        if (slot < 0) {
            mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("NES cache too small"));
        }
        if (self->slot_unit[slot] >= 0) {
            self->utab[self->slot_unit[slot]] = 0;
        }
        self->slot_unit[slot] = -1;
        uint32_t t0 = mp_hal_ticks_us();
        uint8_t *blk = self->slot_buf[slot];
        if (!(self->sd_ok && ne_sd_unit(self, u, blk))) {
            self->sd_ok = false;
            ne_read_at(self, self->hdr_bytes + (u << NE_USHIFT), blk + NE_DOFF, NE_UNIT);
        }
        self->st_load_us += mp_hal_ticks_us() - t0;
        self->st_loads++;
        self->slot_unit[slot] = (int32_t)u;
        self->utab[u] = (uint16_t)(slot + 1);
    }
    self->slot_use[slot] = ++self->tick;
    self->slot_pin[slot] = self->epoch;
    return self->slot_buf[slot] + NE_DOFF;
}

// Called by the core after every bank switch: point all windows into cache slots.
// Windows whose bank did not change only get their slot re-pinned (MMC3 games switch banks
// many times per frame, so this must stay cheap).
static void ne_remap(nes_t *n) {
    ne_obj_t *self = (ne_obj_t *)n->user;
    self->epoch++;
    uint32_t ep = self->epoch;
    for (int i = 0; i < 8; i++) {
        uint32_t off = n->prg_off[i];
        if (off == self->win_off[i] && self->win_slot[i] >= 0 && self->slot_unit[self->win_slot[i]] == (int32_t)(off >> NE_USHIFT)) {
            self->slot_pin[self->win_slot[i]] = ep;
            self->slot_use[self->win_slot[i]] = ++self->tick;
            continue;
        }
        uint8_t *p = ne_load_unit(self, off >> NE_USHIFT);
        n->prg_ptr[i] = p + (off & (NE_UNIT - 1));
        self->win_off[i] = off;
        self->win_slot[i] = (int16_t)(self->utab[off >> NE_USHIFT] - 1);
    }
    for (int i = 0; i < 8; i++) {
        uint32_t off = n->chr_off[i];
        if (off == NES_NOMAP) {
            self->win_off[8 + i] = NES_NOMAP;
            self->win_slot[8 + i] = -1;
            continue;
        }
        uint32_t u = self->nunits_prg + (off >> NE_USHIFT);
        if (off == self->win_off[8 + i] && self->win_slot[8 + i] >= 0 && self->slot_unit[self->win_slot[8 + i]] == (int32_t)u) {
            self->slot_pin[self->win_slot[8 + i]] = ep;
            self->slot_use[self->win_slot[8 + i]] = ++self->tick;
            continue;
        }
        uint8_t *p = ne_load_unit(self, u);
        n->chr_ptr[i] = p + (off & (NE_UNIT - 1));
        self->win_off[8 + i] = off;
        self->win_slot[8 + i] = (int16_t)(self->utab[u] - 1);
    }
}

// ------------------------------------------------------------------ display

static void ne_cmd(ne_obj_t *self, uint8_t cmd, const uint8_t *data, size_t n) {
    gpio_put(self->cs, 0);
    gpio_put(self->dc, 0);
    spi_write_blocking(self->spi, &cmd, 1);
    if (n) {
        gpio_put(self->dc, 1);
        spi_write_blocking(self->spi, data, n);
    }
    gpio_put(self->cs, 1);
}

static void ne_spi_idle(ne_obj_t *self) {
    while (spi_get_hw(self->spi)->sr & SPI_SSPSR_BSY_BITS) {
        tight_loop_contents();
    }
    while (spi_is_readable(self->spi)) {
        (void)spi_get_hw(self->spi)->dr;
    }
    spi_get_hw(self->spi)->icr = SPI_SSPICR_RORIC_BITS;
}

static NES_HOT void ne_start_next(ne_obj_t *self) {
    if (self->q_head == self->q_tail) {
        self->dma_busy = false;
        return;
    }
    int slot = (int)(self->q_head % (uint32_t)self->ring_n);
    int nl = self->s_nl[slot];
    int y0 = self->s_y0[slot];
    uint16_t *buf = self->ring_buf[slot];
    uint8_t d[4];
    int x0 = self->ox;
    int x1 = x0 + NE_W - 1;
    d[0] = x0 >> 8;
    d[1] = x0 & 0xFF;
    d[2] = x1 >> 8;
    d[3] = x1 & 0xFF;
    ne_cmd(self, 0x2A, d, 4);
    int y1 = y0 + nl - 1;
    d[0] = y0 >> 8;
    d[1] = y0 & 0xFF;
    d[2] = y1 >> 8;
    d[3] = y1 & 0xFF;
    ne_cmd(self, 0x2B, d, 4);
    const uint8_t ramwr = 0x2C;
    gpio_put(self->cs, 0);
    gpio_put(self->dc, 0);
    spi_write_blocking(self->spi, &ramwr, 1);
    gpio_put(self->dc, 1);
    dma_channel_config c = dma_channel_get_default_config((uint)ne_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_dreq(&c, spi_get_dreq(self->spi, true));
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    dma_channel_configure((uint)ne_dma, &c, &spi_get_hw(self->spi)->dr, buf, (uint32_t)(nl * NE_W * 2), true);
    self->dma_busy = true;
}

static NES_HOT void ne_dma_irq(void) {
    ne_obj_t *self = ne_active;
    if (self == NULL || !dma_channel_get_irq1_status((uint)ne_dma)) {
        return;
    }
    dma_channel_acknowledge_irq1((uint)ne_dma);
    ne_spi_idle(self);
    gpio_put(self->cs, 1);
    self->q_head++;
    ne_start_next(self);
}

static void ne_dma_wait(ne_obj_t *self) {
    if (!self->hw) {
        return;
    }
    uint32_t t0 = mp_hal_ticks_us();
    bool waited = false;
    while (self->q_head != self->q_tail || self->dma_busy) {
        waited = true;
        tight_loop_contents();
    }
    if (waited) {
        self->st_wait_us += mp_hal_ticks_us() - t0;
    }
}

static NES_HOT void ne_wait_slot(ne_obj_t *self) {
    if (self->q_tail - self->q_head < (uint32_t)self->ring_n) {
        return;
    }
    uint32_t t0 = mp_hal_ticks_us();
    while (self->q_tail - self->q_head >= (uint32_t)self->ring_n) {
        tight_loop_contents();
    }
    self->st_wait_us += mp_hal_ticks_us() - t0;
}

static NES_HOT void ne_flush(ne_obj_t *self) {
    if (self->pending == 0) {
        return;
    }
    int slot = (int)(self->q_tail % (uint32_t)self->ring_n);
    int nl = self->pending;
    int y0 = self->oy + self->pend_y;
    self->pending = 0;
    if (!self->hw) {
        return;
    }
    self->st_strips++;
    self->s_nl[slot] = nl;
    self->s_y0[slot] = y0;
    uint32_t irq = save_and_disable_interrupts();
    self->q_tail++;
    if (!self->dma_busy) {
        ne_start_next(self);
    }
    restore_interrupts(irq);
}

static NES_HOT void ne_line(nes_t *n, int y, const uint8_t *idx) {
    ne_obj_t *self = (ne_obj_t *)n->user;
    if (!self->hw) {
        return;
    }
    if (self->pending == 0) {
        ne_wait_slot(self);
        self->pend_y = y;
    }
    const uint16_t *lut = self->lut;
    uint16_t *row = self->ring_buf[self->q_tail % (uint32_t)self->ring_n] + self->pending * NE_W;
    for (int x = 0; x < NE_W; x += 4) {
        row[x] = lut[idx[x]];
        row[x + 1] = lut[idx[x + 1]];
        row[x + 2] = lut[idx[x + 2]];
        row[x + 3] = lut[idx[x + 3]];
    }
    self->pending++;
    if (self->pending >= NE_STRIP) {
        ne_flush(self);
    }
}

// ------------------------------------------------------------------ Python API

static mp_obj_t ne_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    enum { ARG_file, ARG_slots, ARG_strips };
    static const mp_arg_t allowed[] = {
        {MP_QSTR_file, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_slots, MP_ARG_INT, {.u_int = 24}},
        {MP_QSTR_strips, MP_ARG_INT, {.u_int = 3}},
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, args, MP_ARRAY_SIZE(allowed), allowed, a);

    ne_obj_t *self = m_new_obj(ne_obj_t);
    memset(self, 0, sizeof(*self));
    self->base.type = type;
    self->file = a[ARG_file].u_obj;
    self->stream = mp_get_stream(self->file);
    for (int i = 0; i < 64; i++) {
        uint16_t c = ((ne_rgb[i][0] & 0xF8) << 8) | ((ne_rgb[i][1] & 0xFC) << 3) | (ne_rgb[i][2] >> 3);
        self->lut[i] = (uint16_t)((c >> 8) | (c << 8));     // byte-swapped like st7789.rgb()
    }

    int errcode = 0;
    struct mp_stream_seek_t seek = {.offset = 0, .whence = MP_SEEK_END};
    if (self->stream->ioctl(self->file, MP_STREAM_SEEK, (uintptr_t)&seek, &errcode) == MP_STREAM_ERROR) {
        mp_raise_OSError(errcode);
    }
    self->file_size = (uint32_t)seek.offset;
    if (self->file_size < 16 + 8192) {
        mp_raise_ValueError(MP_ERROR_TEXT("not a NES ROM"));
    }

    uint8_t hdr[16];
    ne_read_at(self, 0, hdr, 16);

    self->nes = m_new(nes_t, 1);
    nes_t *n = self->nes;
    uint32_t trainer = 0;
    int r = nes_load_header(n, hdr, &trainer);
    if (r == 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("not a NES ROM"));
    }
    if (r == 2) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("mapper %d not supported"), (int)n->mapper);
    }
    self->hdr_bytes = 16 + trainer;
    if (self->hdr_bytes + n->prg_size + n->chr_size > self->file_size) {
        // truncated dump: clamp (missing bytes read as 0xFF)
    }
    self->nunits_prg = n->prg_size >> NE_USHIFT;
    self->nunits = self->nunits_prg + (n->chr_size >> NE_USHIFT);
    if (self->nunits > 4096) {
        mp_raise_ValueError(MP_ERROR_TEXT("ROM too big"));
    }

    int slots = a[ARG_slots].u_int;
    slots = slots > NE_MAXS ? NE_MAXS : slots;
    if ((uint32_t)slots > self->nunits) {
        slots = (int)self->nunits;
    }
    int strips = a[ARG_strips].u_int;
    strips = strips < 2 ? 2 : (strips > NE_RMAX ? NE_RMAX : strips);

    self->slot_buf = m_new(uint8_t *, slots > 0 ? slots : 1);
    int got = 0;
    while (got < slots) {
        uint8_t *pg = m_new_maybe(uint8_t, NE_SLOTB);
        if (pg == NULL) {
            break;
        }
        self->slot_buf[got++] = pg;
    }
    int rn = 0;
    while (rn < strips) {
        uint16_t *st = m_new_maybe(uint16_t, NE_STRIP * NE_W);
        if (st == NULL) {
            if (rn < 2 && got > NE_MINSLOTS) {
                self->slot_buf[--got] = NULL;
                gc_collect();
                continue;
            }
            break;
        }
        self->ring_buf[rn++] = st;
    }
    uint32_t need = self->nunits < NE_MINSLOTS ? self->nunits : NE_MINSLOTS;
    if ((uint32_t)got < need || rn < 2) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("not enough RAM for the NES"));
    }
    self->nslots = got;
    self->ring_n = rn;
    self->slot_unit = m_new(int32_t, got);
    self->slot_use = m_new(uint32_t, got);
    self->slot_pin = m_new(uint32_t, got);
    self->utab = m_new(uint16_t, self->nunits);
    memset(self->utab, 0, self->nunits * sizeof(uint16_t));
    for (int i = 0; i < got; i++) {
        self->slot_unit[i] = -1;
        self->slot_use[i] = 0;
        self->slot_pin[i] = 0;
    }
    self->epoch = 1;
    for (int i = 0; i < 16; i++) {
        self->win_off[i] = 0xFFFFFFFFu;
        self->win_slot[i] = -1;
    }

    n->remap = ne_remap;
    n->line_cb = ne_line;
    n->user = self;
    nes_cart_init(n);
    return MP_OBJ_FROM_PTR(self);
}

// sd_direct(spi_id, cs_pin, cdv) -> bool
static mp_obj_t ne_sd_direct(size_t n_args, const mp_obj_t *args) {
    ne_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    self->sd_ok = false;
    if (!mp_obj_is_type(self->file, &mp_type_vfs_fat_fileio)) {
        return mp_const_false;
    }
    FIL *fp = &((ne_fat_file_t *)MP_OBJ_TO_PTR(self->file))->fp;
    FATFS *fs = fp->obj.fs;
    if (fs == NULL) {
        return mp_const_false;
    }
    uint32_t ss = 512;
    #if FF_MAX_SS != FF_MIN_SS
    ss = fs->ssize;
    #endif
    uint32_t cl_bytes = (uint32_t)fs->csize * ss;
    if (ss != 512) {
        return mp_const_false;
    }
    self->sd_spi = mp_obj_get_int(args[1]) ? spi1 : spi0;
    self->sd_cs = mp_obj_get_int(args[2]);
    self->sd_cdv = mp_obj_get_int(args[3]);
    self->sd_cl_bytes = cl_bytes;

    uint32_t ncl = (self->file_size + cl_bytes - 1) / cl_bytes;
    uint32_t *tab = m_new(uint32_t, ncl);
    for (uint32_t i = 0; i < ncl; i++) {
        if (f_lseek(fp, (FSIZE_t)i * cl_bytes + 1) != FR_OK || fp->sect == 0) {
            return mp_const_false;
        }
        tab[i] = (uint32_t)fp->sect;
    }
    self->sd_table = tab;

    // verify unit 0 and the last unit against the normal path, using cache slots 0 and 1 as scratch
    if (self->nslots < 2) {
        return mp_const_false;
    }
    for (int i = 0; i < 2; i++) {
        if (self->slot_unit[i] >= 0) {
            self->utab[self->slot_unit[i]] = 0;
            self->slot_unit[i] = -1;
        }
    }
    bool ok = true;
    uint32_t chk[2] = {0, self->nunits - 1};
    uint32_t avail = self->file_size - self->hdr_bytes;
    for (int k = 0; k < 2 && ok; k++) {
        if (k == 1 && chk[1] == 0) {
            break;
        }
        uint32_t off = chk[k] << NE_USHIFT;
        uint32_t cmp = (off + NE_UNIT <= avail) ? NE_UNIT : (off < avail ? avail - off : 0);
        ok = ne_sd_unit(self, chk[k], self->slot_buf[0]);
        if (ok) {
            ne_read_at(self, self->hdr_bytes + off, self->slot_buf[1] + NE_DOFF, NE_UNIT);
            ok = memcmp(self->slot_buf[0] + NE_DOFF, self->slot_buf[1] + NE_DOFF, cmp) == 0;
        }
    }
    self->sd_ok = ok;
    for (int i = 0; i < 16; i++) {
        self->win_off[i] = 0xFFFFFFFFu;
    }
    ne_remap(self->nes);   // windows may have pointed into the scratch slots
    return mp_obj_new_bool(ok);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ne_sd_direct_obj, 4, 4, ne_sd_direct);

// display_hw(spi_id, dc_gpio, cs_gpio, x, y, mode, baud) - mode is ignored (always 256x240)
static mp_obj_t ne_display_hw(size_t n_args, const mp_obj_t *args) {
    ne_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    ne_dma_wait(self);
    int spi_id = mp_obj_get_int(args[1]);
    if (spi_id < 0 || spi_id > 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad SPI id"));
    }
    if (ne_dma < 0) {
        int ch = dma_claim_unused_channel(false);
        if (ch < 0) {
            mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("no free DMA channel"));
        }
        ne_dma = ch;
    }
    self->spi = spi_id ? spi1 : spi0;
    self->dc = (unsigned)mp_obj_get_int(args[2]);
    self->cs = (unsigned)mp_obj_get_int(args[3]);
    self->ox = mp_obj_get_int(args[4]);
    self->oy = mp_obj_get_int(args[5]);
    int baud = mp_obj_get_int(args[7]);
    uint32_t sysf = clock_get_hz(clk_sys);
    if (clock_get_hz(clk_peri) != sysf) {
        clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, sysf, sysf);
    }
    if (baud > 0) {
        spi_set_baudrate(self->spi, (unsigned)baud);
    }
    if (!ne_irq_ready) {
        irq_add_shared_handler(DMA_IRQ_1, ne_dma_irq, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
        ne_irq_ready = true;
    }
    ne_active = self;
    dma_channel_acknowledge_irq1((uint)ne_dma);
    dma_channel_set_irq1_enabled((uint)ne_dma, true);
    irq_set_enabled(DMA_IRQ_1, true);
    self->q_head = self->q_tail = 0;
    self->hw = true;
    self->pending = 0;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ne_display_hw_obj, 8, 8, ne_display_hw);

static mp_obj_t ne_options(mp_obj_t self_in, mp_obj_t frame_skip) {
    ne_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->frame_skip = mp_obj_is_true(frame_skip);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ne_options_obj, ne_options);

static mp_obj_t ne_frame(size_t n_args, const mp_obj_t *args) {
    ne_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    uint32_t m = (uint32_t)mp_obj_get_int(args[1]);
    bool draw = n_args < 3 ? true : mp_obj_is_true(args[2]);
    if (self->frame_skip) {
        self->skip_phase = !self->skip_phase;
        if (self->skip_phase) {
            draw = false;
        }
    }
    uint8_t k = 0;
    if (m & BT_UP) k |= NES_UP;
    if (m & BT_DOWN) k |= NES_DOWN;
    if (m & BT_LEFT) k |= NES_LEFT;
    if (m & BT_RIGHT) k |= NES_RIGHT;
    if (m & BT_A) k |= NES_A;
    if (m & BT_B) k |= NES_B;
    if (m & BT_START) k |= NES_START;
    if (m & BT_SELECT) k |= NES_SELECT;
    self->pending = 0;
    nes_frame(self->nes, k, draw && self->hw);
    ne_flush(self);
    if (self->hw && self->st_strips) {
        self->st_drawn++;
    }
    self->st_strips = 0;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ne_frame_obj, 2, 3, ne_frame);

static mp_obj_t ne_sync(mp_obj_t self_in) {
    ne_obj_t *self = MP_OBJ_TO_PTR(self_in);
    ne_dma_wait(self);
    self->pending = 0;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ne_sync_obj, ne_sync);

static mp_obj_t ne_info(mp_obj_t self_in) {
    ne_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_t t[2] = {MP_OBJ_NEW_SMALL_INT(self->nslots), MP_OBJ_NEW_SMALL_INT(self->ring_n)};
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ne_info_obj, ne_info);

static mp_obj_t ne_stats(mp_obj_t self_in) {
    ne_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_t t[6];
    t[5] = mp_obj_new_int_from_uint(clock_get_hz(clk_peri) / 1000000);
    t[3] = mp_obj_new_int_from_uint(self->st_drawn);
    t[4] = mp_obj_new_int_from_uint(self->hw ? spi_get_baudrate(self->spi) : 0);
    self->st_drawn = 0;
    t[0] = mp_obj_new_int_from_uint(self->st_loads);
    t[1] = mp_obj_new_int_from_uint(self->st_load_us);
    t[2] = mp_obj_new_int_from_uint(self->st_wait_us);
    self->st_loads = 0;
    self->st_load_us = 0;
    self->st_wait_us = 0;
    return mp_obj_new_tuple(6, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ne_stats_obj, ne_stats);

// ram() -> memoryview of the 8 KB PRG-RAM (battery save) or None when the cartridge has no battery
static mp_obj_t ne_ram(mp_obj_t self_in) {
    ne_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->nes->battery) {
        return mp_const_none;
    }
    return mp_obj_new_memoryview('B' | MP_OBJ_ARRAY_TYPECODE_FLAG_RW, sizeof(self->nes->prg_ram), self->nes->prg_ram);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ne_ram_obj, ne_ram);

static mp_obj_t ne_dirty(mp_obj_t self_in) {
    ne_obj_t *self = MP_OBJ_TO_PTR(self_in);
    bool d = self->nes->ram_dirty;
    self->nes->ram_dirty = false;
    return mp_obj_new_bool(d);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ne_dirty_obj, ne_dirty);

static mp_obj_t ne_mapper(mp_obj_t self_in) {
    ne_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->nes->mapper);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ne_mapper_obj, ne_mapper);

static mp_obj_t ne_title(mp_obj_t self_in) {
    ne_obj_t *self = MP_OBJ_TO_PTR(self_in);
    char buf[24] = "NES mapper ";
    int len = 11;
    unsigned m = self->nes->mapper;
    if (m >= 100) {
        buf[len++] = '0' + m / 100;
    }
    if (m >= 10) {
        buf[len++] = '0' + (m / 10) % 10;
    }
    buf[len++] = '0' + m % 10;
    return mp_obj_new_str(buf, len);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ne_title_obj, ne_title);

static const mp_rom_map_elem_t ne_locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&ne_info_obj)},
    {MP_ROM_QSTR(MP_QSTR_sd_direct), MP_ROM_PTR(&ne_sd_direct_obj)},
    {MP_ROM_QSTR(MP_QSTR_display_hw), MP_ROM_PTR(&ne_display_hw_obj)},
    {MP_ROM_QSTR(MP_QSTR_options), MP_ROM_PTR(&ne_options_obj)},
    {MP_ROM_QSTR(MP_QSTR_frame), MP_ROM_PTR(&ne_frame_obj)},
    {MP_ROM_QSTR(MP_QSTR_sync), MP_ROM_PTR(&ne_sync_obj)},
    {MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&ne_stats_obj)},
    {MP_ROM_QSTR(MP_QSTR_ram), MP_ROM_PTR(&ne_ram_obj)},
    {MP_ROM_QSTR(MP_QSTR_dirty), MP_ROM_PTR(&ne_dirty_obj)},
    {MP_ROM_QSTR(MP_QSTR_title), MP_ROM_PTR(&ne_title_obj)},
    {MP_ROM_QSTR(MP_QSTR_mapper), MP_ROM_PTR(&ne_mapper_obj)},
};
static MP_DEFINE_CONST_DICT(ne_locals_dict, ne_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    ne_type,
    MP_QSTR_Emu,
    MP_TYPE_FLAG_NONE,
    make_new, ne_make_new,
    locals_dict, &ne_locals_dict
    );

static const mp_rom_map_elem_t nesemu_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_nesemu)},
    {MP_ROM_QSTR(MP_QSTR_Emu), MP_ROM_PTR(&ne_type)},
};
static MP_DEFINE_CONST_DICT(nesemu_globals, nesemu_globals_table);

const mp_obj_module_t nesemu_user_cmodule = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&nesemu_globals,
};

MP_REGISTER_MODULE(MP_QSTR_nesemu, nesemu_user_cmodule);
