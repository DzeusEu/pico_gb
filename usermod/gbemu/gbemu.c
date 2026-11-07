// gbemu - MicroPython user C module wrapping the Peanut-GB Game Boy emulator core.
// Peanut-GB (c) Mahyar Koshkouei, MIT license: https://github.com/deltabeard/Peanut-GB
//
//   import gbemu
//   emu = gbemu.Emu(open("game.gb", "rb"), slots)      # ROM stays on the SD card
//   emu.display_hw(spi_id, dc, cs, x, y, mode, baud)   # stream frames to the ST7789 by DMA
//   emu.display(blit, x, y, mode)                      # (fallback) call a Python blit function
//   emu.palette(c0, c1, c2, c3)                        # 4 shades, lightest first
//   emu.options(frame_skip)                            # draw only every other frame
//   emu.frame(buttons, draw=True)                      # run one frame (~16.7 ms of game time)
//   emu.sync()                                         # wait until the display transfer is done
//   emu.stats()                                        # (rom bank loads, load us, display wait us)
//   emu.ram() / emu.dirty() / emu.title()
//
// mode 0 = 160x144, mode 1 = 240x216 (1.5x).
//
// Display path: finished lines are collected in two small strip buffers. While one strip is
// sent to the screen by DMA, the CPU keeps emulating and fills the other one, so the (slow)
// SPI transfer no longer stalls the emulator.
//
// ROM is cached in RAM (bank 0 always resident, plus `slots` x 4 KB pages with LRU
// replacement) and fetched from the file on demand, so ROMs far bigger than RAM work.

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
#include "hardware/sync.h"

#define ENABLE_LCD 1
#define ENABLE_SOUND 0
#define PEANUT_GB_HIGH_LCD_ACCURACY 0
#include "peanut_gb.h"

#define GBE_BANK 16384
#define GBE_PAGE 4096           // ROM cache page (4 KB: cheaper SD reads, finer-grained cache)
#define GBE_PSHIFT 12
#define GBE_MAXS 250            // max cache pages
#define GBE_STRIP 8             // lines per display strip buffer
#define GBE_RMAX 24             // max strips in the display queue
#define GBE_HOT __attribute__((section(".time_critical.gbemu")))  // run from RAM, not flash
#define GBE_MAX_RAM 32768

// button bits expected from Python (same as buttons.py)
#define BT_UP 1
#define BT_DOWN 2
#define BT_LEFT 4
#define BT_RIGHT 8
#define BT_A 16
#define BT_B 32
#define BT_START 64
#define BT_SELECT 128

typedef struct _gbe_obj_t {
    mp_obj_base_t base;
    struct gb_s gb;
    mp_obj_t file;
    const mp_stream_p_t *stream;
    mp_obj_t draw;
    uint32_t rom_size;
    uint8_t *bank0;
    uint8_t *cache;
    int nslots;
    int32_t *slot_page;       // page held by each cache slot (-1 = empty)
    uint32_t *slot_use;
    uint16_t *ptab;           // page number -> slot + 1 (0 = not cached)
    uint32_t npages;
    uint32_t tick;
    int32_t last_page;
    uint8_t *last_ptr;
    uint8_t *ram;
    uint32_t ram_size;
    bool dirty;
    // colour lookup, index = pixel & 0x33: bits 5-4 select the layer (0 OBJ0, 1 OBJ1, 2 BG),
    // bits 1-0 the shade. Lets a DMG game be coloured like the Game Boy Color did.
    uint16_t lut[64];
    int ox, oy, mode;
    bool draw_on;
    int pending, pend_y;
    bool hw;
    volatile bool dma_busy;
    // display queue: rendered strips wait in `ring` and a DMA interrupt sends them one after
    // another, so the emulator can carry on (even into the next frame) while the screen is fed
    uint16_t *ring;
    int ring_n;
    int s_nl[GBE_RMAX], s_y0[GBE_RMAX];
    volatile uint32_t q_head, q_tail;
    spi_inst_t *spi;
    unsigned dc, cs;
    // direct SD reads (bypass the Python SD driver): see gbe_sd_direct()
    bool sd_ok, sd_contig;
    spi_inst_t *sd_spi;
    unsigned sd_cs;
    uint32_t sd_cdv, sd_base, sd_cl_bytes, sd_cl_sec;
    uint32_t *sd_table;
    uint32_t st_loads, st_load_us, st_wait_us, st_drawn, st_strips;
} gbe_obj_t;

extern const mp_obj_type_t gbe_type;

// one DMA channel, claimed on first use and kept for the life of the firmware
static int gbe_dma = -1;

// ------------------------------------------------------------------ ROM access

static void gbe_read_at(gbe_obj_t *self, uint32_t offset, uint8_t *dst, uint32_t len) {
    int errcode = 0;
    struct mp_stream_seek_t seek = {.offset = (mp_off_t)offset, .whence = MP_SEEK_SET};
    mp_uint_t r = self->stream->ioctl(self->file, MP_STREAM_SEEK, (uintptr_t)&seek, &errcode);
    if (r == MP_STREAM_ERROR) {
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
// The ROM file's sectors on the card are looked up once (from FatFS's own file object), then
// pages are read with CMD18 straight over the SD card's SPI bus from C. This avoids the Python
// SD driver (about 3 ms of interpreter overhead per 4 KB). Any problem -> falls back to the
// normal file reads.

typedef struct _gbe_fat_file_t {   // same layout as pyb_file_obj_t in extmod/vfs_fat_file.c
    mp_obj_base_t base;
    FIL fp;
} gbe_fat_file_t;

static bool gbe_sd_cmd12(gbe_obj_t *self) {
    uint8_t c[6] = {0x4C, 0, 0, 0, 0, 0xFF};
    uint8_t b = 0xFF;
    spi_write_blocking(self->sd_spi, c, 6);
    spi_read_blocking(self->sd_spi, 0xFF, &b, 1);  // stuff byte
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

static bool gbe_sd_read(gbe_obj_t *self, uint32_t sector, uint8_t *dst, uint32_t nsec) {
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
    if (!gbe_sd_cmd12(self)) {
        ok = false;
    }
    gpio_put(self->sd_cs, 1);
    uint8_t ff = 0xFF;
    spi_write_blocking(spi, &ff, 1);
    return ok;
}

// read one page of the ROM; false -> caller must use the normal path
static bool gbe_sd_page(gbe_obj_t *self, uint32_t page, uint8_t *dst) {
    uint32_t off = page << GBE_PSHIFT;
    if (off >= self->rom_size) {
        return false;
    }
    uint32_t sec;
    if (self->sd_contig) {
        sec = self->sd_base + (off >> 9);
    } else {
        sec = self->sd_table[off / self->sd_cl_bytes] + ((off % self->sd_cl_bytes) >> 9);
    }
    if (!gbe_sd_read(self, sec, dst, GBE_PAGE / 512)) {
        return false;
    }
    if (off + GBE_PAGE > self->rom_size) {
        memset(dst + (self->rom_size - off), 0xFF, off + GBE_PAGE - self->rom_size);
    }
    return true;
}

static uint8_t *gbe_load_page(gbe_obj_t *self, uint32_t page) {
    int slot = (int)self->ptab[page] - 1;
    if (slot < 0) {
        uint32_t oldest = 0xFFFFFFFFu;
        for (int i = 0; i < self->nslots; i++) {
            if (self->slot_page[i] < 0) {
                slot = i;
                break;
            }
            if (self->slot_use[i] < oldest) {
                oldest = self->slot_use[i];
                slot = i;
            }
        }
        if (self->slot_page[slot] >= 0) {
            self->ptab[self->slot_page[slot]] = 0;
            if (self->last_page == self->slot_page[slot]) {
                self->last_page = -1;
            }
        }
        self->slot_page[slot] = -1;  // stays invalid if the read below raises
        uint32_t t0 = mp_hal_ticks_us();
        uint8_t *dst = self->cache + (size_t)slot * GBE_PAGE;
        if (!(self->sd_ok && gbe_sd_page(self, page, dst))) {
            self->sd_ok = false;
            gbe_read_at(self, page << GBE_PSHIFT, dst, GBE_PAGE);
        }
        self->st_load_us += mp_hal_ticks_us() - t0;
        self->st_loads++;
        self->slot_page[slot] = (int32_t)page;
        self->ptab[page] = (uint16_t)(slot + 1);
    }
    self->slot_use[slot] = ++self->tick;
    self->last_page = (int32_t)page;
    self->last_ptr = self->cache + (size_t)slot * GBE_PAGE;
    return self->last_ptr;
}

// ------------------------------------------------------------------ Peanut-GB callbacks

static GBE_HOT uint8_t gbe_rom_read(struct gb_s *gb, const uint_fast32_t addr) {
    gbe_obj_t *self = (gbe_obj_t *)gb->direct.priv;
    if (addr < GBE_BANK) {
        return self->bank0[addr];
    }
    uint32_t page = (uint32_t)(addr >> GBE_PSHIFT);
    if ((int32_t)page == self->last_page) {
        return self->last_ptr[addr & (GBE_PAGE - 1)];
    }
    if (page >= self->npages) {
        return 0xFF;
    }
    uint32_t slot = self->ptab[page];
    if (slot) {
        self->slot_use[slot - 1] = ++self->tick;
        self->last_page = (int32_t)page;
        self->last_ptr = self->cache + (size_t)(slot - 1) * GBE_PAGE;
        return self->last_ptr[addr & (GBE_PAGE - 1)];
    }
    return gbe_load_page(self, page)[addr & (GBE_PAGE - 1)];
}

static GBE_HOT uint8_t gbe_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr) {
    gbe_obj_t *self = (gbe_obj_t *)gb->direct.priv;
    return addr < self->ram_size ? self->ram[addr] : 0xFF;
}

static GBE_HOT void gbe_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val) {
    gbe_obj_t *self = (gbe_obj_t *)gb->direct.priv;
    if (addr < self->ram_size && self->ram[addr] != val) {
        self->ram[addr] = val;
        self->dirty = true;
    }
}

static void gbe_error(struct gb_s *gb, const enum gb_error_e err, const uint16_t val) {
    // Bad reads/writes are survivable (Peanut-GB continues); an invalid opcode is not.
    if (err == GB_INVALID_OPCODE || err == GB_UNKNOWN_ERROR) {
        mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("GB error %d (0x%04x)"), (int)err, (unsigned)val);
    }
}

// ------------------------------------------------------------------ display output

static gbe_obj_t *gbe_active = NULL;
static bool gbe_irq_ready = false;

static void gbe_cmd(gbe_obj_t *self, uint8_t cmd, const uint8_t *data, size_t n) {
    gpio_put(self->cs, 0);
    gpio_put(self->dc, 0);
    spi_write_blocking(self->spi, &cmd, 1);
    if (n) {
        gpio_put(self->dc, 1);
        spi_write_blocking(self->spi, data, n);
    }
    gpio_put(self->cs, 1);
}

static void gbe_spi_idle(gbe_obj_t *self) {
    while (spi_get_hw(self->spi)->sr & SPI_SSPSR_BSY_BITS) {
        tight_loop_contents();
    }
    while (spi_is_readable(self->spi)) {
        (void)spi_get_hw(self->spi)->dr;
    }
    spi_get_hw(self->spi)->icr = SPI_SSPICR_RORIC_BITS;
}

// Start sending the oldest queued strip (call with interrupts off or from the DMA handler).
static GBE_HOT void gbe_start_next(gbe_obj_t *self) {
    if (self->q_head == self->q_tail) {
        self->dma_busy = false;
        return;
    }
    int slot = (int)(self->q_head % (uint32_t)self->ring_n);
    int outw = self->mode == 0 ? 160 : 240;
    int nl = self->s_nl[slot];
    int y0 = self->s_y0[slot];
    uint16_t *buf = self->ring + (size_t)slot * GBE_STRIP * 240;
    uint8_t d[4];
    int x0 = self->ox;
    int x1 = x0 + outw - 1;
    d[0] = x0 >> 8;
    d[1] = x0 & 0xFF;
    d[2] = x1 >> 8;
    d[3] = x1 & 0xFF;
    gbe_cmd(self, 0x2A, d, 4);
    int y1 = y0 + nl - 1;
    d[0] = y0 >> 8;
    d[1] = y0 & 0xFF;
    d[2] = y1 >> 8;
    d[3] = y1 & 0xFF;
    gbe_cmd(self, 0x2B, d, 4);
    const uint8_t ramwr = 0x2C;
    gpio_put(self->cs, 0);
    gpio_put(self->dc, 0);
    spi_write_blocking(self->spi, &ramwr, 1);
    gpio_put(self->dc, 1);
    dma_channel_config c = dma_channel_get_default_config((uint)gbe_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_dreq(&c, spi_get_dreq(self->spi, true));
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    dma_channel_configure((uint)gbe_dma, &c, &spi_get_hw(self->spi)->dr, buf,
        (uint32_t)(nl * outw * 2), true);
    self->dma_busy = true;
}

static GBE_HOT void gbe_dma_irq(void) {
    gbe_obj_t *self = gbe_active;
    if (self == NULL || !dma_channel_get_irq1_status((uint)gbe_dma)) {
        return;
    }
    dma_channel_acknowledge_irq1((uint)gbe_dma);
    gbe_spi_idle(self);          // last bytes shifted out
    gpio_put(self->cs, 1);
    self->q_head++;
    gbe_start_next(self);
}

// Wait until every queued strip has been sent and the bus is idle.
static void gbe_dma_wait(gbe_obj_t *self) {
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

// Producer: make sure the strip we are about to render into is not still queued.
static GBE_HOT void gbe_wait_slot(gbe_obj_t *self) {
    if (self->q_tail - self->q_head < (uint32_t)self->ring_n) {
        return;
    }
    uint32_t t0 = mp_hal_ticks_us();
    while (self->q_tail - self->q_head >= (uint32_t)self->ring_n) {
        tight_loop_contents();
    }
    self->st_wait_us += mp_hal_ticks_us() - t0;
}

static GBE_HOT void gbe_flush(gbe_obj_t *self) {
    if (self->pending == 0) {
        return;
    }
    int outw = self->mode == 0 ? 160 : 240;
    int slot = (int)(self->q_tail % (uint32_t)self->ring_n);
    uint16_t *buf = self->ring + (size_t)slot * GBE_STRIP * 240;
    int nl = self->pending;
    int y0 = self->oy + self->pend_y;
    self->pending = 0;

    if (self->hw) {
        self->st_strips++;
        self->s_nl[slot] = nl;
        self->s_y0[slot] = y0;
        uint32_t irq = save_and_disable_interrupts();
        self->q_tail++;
        if (!self->dma_busy) {
            gbe_start_next(self);
        }
        restore_interrupts(irq);
        return;
    }

    // fallback: call the Python blit function (blocking)
    mp_obj_t args[5];
    args[0] = mp_obj_new_memoryview('B', (size_t)nl * outw * 2, buf);
    args[1] = MP_OBJ_NEW_SMALL_INT(self->ox);
    args[2] = MP_OBJ_NEW_SMALL_INT(y0);
    args[3] = MP_OBJ_NEW_SMALL_INT(outw);
    args[4] = MP_OBJ_NEW_SMALL_INT(nl);
    mp_call_function_n_kw(self->draw, 5, 0, args);
}

static GBE_HOT void gbe_lcd_line(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line) {
    gbe_obj_t *self = (gbe_obj_t *)gb->direct.priv;
    if (!self->draw_on || (!self->hw && self->draw == mp_const_none)) {
        return;
    }
    if (self->pending == 0 && self->hw) {
        gbe_wait_slot(self);
    }
    const uint16_t *lut = self->lut;
    uint16_t *base = self->ring + (size_t)(self->hw ? self->q_tail % (uint32_t)self->ring_n : 0) * GBE_STRIP * 240;
    if (self->mode == 0) {
        if (self->pending == 0) {
            self->pend_y = line;
        }
        uint16_t *row = base + self->pending * 160;
        for (int x = 0; x < 160; x++) {
            row[x] = lut[pixels[x] & 0x33];
        }
        self->pending++;
    } else {
        // 1.5x nearest neighbour: 160x144 -> 240x216 (every 2 source lines give 3 output lines)
        int start = (3 * (int)line + 1) / 2;
        int count = (line & 1) ? 1 : 2;
        if (self->pending == 0) {
            self->pend_y = start;
        }
        uint16_t *row = base + self->pending * 240;
        int o = 0;
        for (int x = 0; x < 160; x += 2) {
            uint16_t c0 = lut[pixels[x] & 0x33];
            uint16_t c1 = lut[pixels[x + 1] & 0x33];
            row[o++] = c0;
            row[o++] = c0;
            row[o++] = c1;
        }
        if (count == 2) {
            memcpy(row + 240, row, 240 * sizeof(uint16_t));
        }
        self->pending += count;
    }
    if (self->pending + 2 > GBE_STRIP) {
        gbe_flush(self);
    }
}

// ------------------------------------------------------------------ helpers

static uint32_t gbe_ram_size(const uint8_t *hdr) {
    static const uint32_t sizes[6] = {0, 2048, 8192, 32768, 131072, 65536};
    uint8_t type = hdr[0x147];
    if (type == 5 || type == 6) {
        return 512;  // MBC2 internal RAM
    }
    return hdr[0x149] < 6 ? sizes[hdr[0x149]] : 0;
}

// ------------------------------------------------------------------ Python API

static mp_obj_t gbe_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    enum { ARG_file, ARG_slots, ARG_strips };
    static const mp_arg_t allowed[] = {
        {MP_QSTR_file, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_slots, MP_ARG_INT, {.u_int = 16}},
        {MP_QSTR_strips, MP_ARG_INT, {.u_int = 2}},
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, args, MP_ARRAY_SIZE(allowed), allowed, a);

    int slots = a[ARG_slots].u_int;
    if (slots < 1) {
        slots = 1;
    }
    if (slots > GBE_MAXS) {
        slots = GBE_MAXS;
    }

    gbe_obj_t *self = m_new_obj(gbe_obj_t);
    memset(self, 0, sizeof(*self));
    self->base.type = type;
    self->file = a[ARG_file].u_obj;
    self->stream = mp_get_stream(self->file);
    self->draw = mp_const_none;
    self->nslots = slots;
    self->last_page = -1;
    self->mode = 1;
    static const uint16_t shades[4] = {0xFFFF, 0xAAAA, 0x5555, 0x0000};
    for (int s = 0; s < 4; s++) {
        for (int c = 0; c < 4; c++) {
            self->lut[(s << 4) | c] = shades[c];
        }
    }

    // ROM size
    int errcode = 0;
    struct mp_stream_seek_t seek = {.offset = 0, .whence = MP_SEEK_END};
    if (self->stream->ioctl(self->file, MP_STREAM_SEEK, (uintptr_t)&seek, &errcode) == MP_STREAM_ERROR) {
        mp_raise_OSError(errcode);
    }
    self->rom_size = (uint32_t)seek.offset;
    if (self->rom_size < 0x150) {
        mp_raise_ValueError(MP_ERROR_TEXT("not a Game Boy ROM"));
    }

    // buffers (GC heap; freed when the object is collected)
    self->bank0 = m_new(uint8_t, GBE_BANK);
    self->npages = (self->rom_size + GBE_PAGE - 1) >> GBE_PSHIFT;
    if (self->npages > 4096) {
        self->npages = 4096;
    }
    if ((uint32_t)slots > self->npages) {
        slots = (int)self->npages;
        self->nslots = slots;
    }
    int strips = a[ARG_strips].u_int;
    strips = strips < 2 ? 2 : (strips > GBE_RMAX ? GBE_RMAX : strips);
    self->ring_n = strips;
    self->ring = m_new(uint16_t, (size_t)strips * GBE_STRIP * 240);
    self->cache = m_new(uint8_t, (size_t)slots * GBE_PAGE);
    self->slot_page = m_new(int32_t, slots);
    self->slot_use = m_new(uint32_t, slots);
    self->ptab = m_new(uint16_t, self->npages);
    memset(self->ptab, 0, self->npages * sizeof(uint16_t));
    for (int i = 0; i < slots; i++) {
        self->slot_page[i] = -1;
        self->slot_use[i] = 0;
    }
    gbe_read_at(self, 0, self->bank0, GBE_BANK);

    self->ram_size = gbe_ram_size(self->bank0);
    if (self->ram_size > GBE_MAX_RAM) {
        self->ram_size = GBE_MAX_RAM;
    }
    if (self->ram_size) {
        self->ram = m_new(uint8_t, self->ram_size);
        memset(self->ram, 0, self->ram_size);
    }

    enum gb_init_error_e e = gb_init(&self->gb, gbe_rom_read, gbe_cart_ram_read,
        gbe_cart_ram_write, gbe_error, self);
    if (e == GB_INIT_CARTRIDGE_UNSUPPORTED) {
        mp_raise_ValueError(MP_ERROR_TEXT("cartridge type not supported"));
    } else if (e != GB_INIT_NO_ERROR) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid ROM (header checksum)"));
    }
    gb_init_lcd(&self->gb, gbe_lcd_line);
    return MP_OBJ_FROM_PTR(self);
}

// sd_direct(spi_id, cs_pin, cdv) -> bool: switch ROM page reads to direct SD access.
// Returns False (and keeps the normal path) if anything does not check out.
static mp_obj_t gbe_sd_direct(size_t n_args, const mp_obj_t *args) {
    gbe_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    self->sd_ok = false;
    if (!mp_obj_is_type(self->file, &mp_type_vfs_fat_fileio)) {
        return mp_const_false;
    }
    FIL *fp = &((gbe_fat_file_t *)MP_OBJ_TO_PTR(self->file))->fp;
    FATFS *fs = fp->obj.fs;
    if (fs == NULL) {
        return mp_const_false;
    }
    uint32_t ss = 512;
    #if FF_MAX_SS != FF_MIN_SS
    ss = fs->ssize;
    #endif
    uint32_t cl_bytes = (uint32_t)fs->csize * ss;
    if (ss != 512 || cl_bytes < GBE_PAGE || (cl_bytes & (GBE_PAGE - 1))) {
        return mp_const_false;
    }
    self->sd_spi = mp_obj_get_int(args[1]) ? spi1 : spi0;
    self->sd_cs = mp_obj_get_int(args[2]);
    self->sd_cdv = mp_obj_get_int(args[3]);
    self->sd_cl_bytes = cl_bytes;

    // sector of every cluster of the file (f_lseek to byte 1 of a cluster leaves fp->sect = its first sector)
    uint32_t ncl = (self->rom_size + cl_bytes - 1) / cl_bytes;
    uint32_t *tab = m_new(uint32_t, ncl);
    for (uint32_t i = 0; i < ncl; i++) {
        if (f_lseek(fp, (FSIZE_t)i * cl_bytes + 1) != FR_OK || fp->sect == 0) {
            return mp_const_false;
        }
        tab[i] = (uint32_t)fp->sect;
    }
    bool contig = true;
    for (uint32_t i = 1; i < ncl; i++) {
        if (tab[i] != tab[0] + i * fs->csize) {
            contig = false;
            break;
        }
    }
    self->sd_contig = contig;
    self->sd_base = tab[0];
    self->sd_table = contig ? NULL : tab;

    // verify against the normal path: first page (bank 0 is already loaded) and, if there
    // is room for a second buffer, the last page
    uint8_t *tmp = self->cache;
    if (!gbe_sd_page(self, 0, tmp) || memcmp(tmp, self->bank0, GBE_PAGE) != 0) {
        return mp_const_false;
    }
    if (self->npages > 1 && self->nslots >= 2) {
        uint32_t pg = self->npages - 1;
        if (!gbe_sd_page(self, pg, tmp)) {
            return mp_const_false;
        }
        gbe_read_at(self, pg << GBE_PSHIFT, tmp + GBE_PAGE, GBE_PAGE);
        if (memcmp(tmp, tmp + GBE_PAGE, GBE_PAGE) != 0) {
            return mp_const_false;
        }
    }
    self->sd_ok = true;
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gbe_sd_direct_obj, 4, 4, gbe_sd_direct);

// display(callback, x, y, mode) - Python blit fallback: callback(buf, x, y, w, h)
static mp_obj_t gbe_display(size_t n_args, const mp_obj_t *args) {
    gbe_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    gbe_dma_wait(self);
    self->hw = false;
    self->draw = args[1];
    self->ox = mp_obj_get_int(args[2]);
    self->oy = mp_obj_get_int(args[3]);
    self->mode = mp_obj_get_int(args[4]) ? 1 : 0;
    self->pending = 0;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gbe_display_obj, 5, 5, gbe_display);

// display_hw(spi_id, dc_gpio, cs_gpio, x, y, mode, baud) - DMA straight to the ST7789.
// baud > 0 sets the SPI clock of that bus; call again after changing machine.freq().
static mp_obj_t gbe_display_hw(size_t n_args, const mp_obj_t *args) {
    gbe_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    gbe_dma_wait(self);
    int spi_id = mp_obj_get_int(args[1]);
    if (spi_id < 0 || spi_id > 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad SPI id"));
    }
    if (gbe_dma < 0) {
        int ch = dma_claim_unused_channel(false);
        if (ch < 0) {
            mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("no free DMA channel"));
        }
        gbe_dma = ch;
    }
    self->spi = spi_id ? spi1 : spi0;
    self->dc = (unsigned)mp_obj_get_int(args[2]);
    self->cs = (unsigned)mp_obj_get_int(args[3]);
    self->ox = mp_obj_get_int(args[4]);
    self->oy = mp_obj_get_int(args[5]);
    self->mode = mp_obj_get_int(args[6]) ? 1 : 0;
    int baud = mp_obj_get_int(args[7]);
    if (baud > 0) {
        spi_set_baudrate(self->spi, (unsigned)baud);
    }
    if (!gbe_irq_ready) {
        irq_add_shared_handler(DMA_IRQ_1, gbe_dma_irq, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
        gbe_irq_ready = true;
    }
    gbe_active = self;
    dma_channel_acknowledge_irq1((uint)gbe_dma);
    dma_channel_set_irq1_enabled((uint)gbe_dma, true);
    irq_set_enabled(DMA_IRQ_1, true);
    self->q_head = self->q_tail = 0;
    self->hw = true;
    self->pending = 0;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gbe_display_hw_obj, 8, 8, gbe_display_hw);

// palette(bg0..bg3)                          - one 4-shade palette for everything
// palette(bg0..bg3, o0_0..o0_3, o1_0..o1_3)  - separate background / sprite palette 0 / sprite palette 1
// shades are lightest first, values as produced by st7789.rgb()
static mp_obj_t gbe_palette(size_t n_args, const mp_obj_t *args) {
    gbe_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (n_args != 5 && n_args != 13) {
        mp_raise_ValueError(MP_ERROR_TEXT("palette needs 4 or 12 colours"));
    }
    uint16_t c[12];
    int n = (int)n_args - 1;
    for (int i = 0; i < n; i++) {
        c[i] = (uint16_t)mp_obj_get_int(args[1 + i]);
    }
    for (int s = 0; s < 4; s++) {
        // layer index -> colours: 0 = OBJ0, 1 = OBJ1, 2 = BG (3 is undefined; use BG)
        int src = 0;  // offset into c[]
        if (n == 12) {
            src = (s == 0) ? 4 : (s == 1) ? 8 : 0;
        }
        for (int k = 0; k < 4; k++) {
            self->lut[(s << 4) | k] = c[src + k];
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gbe_palette_obj, 5, 13, gbe_palette);

// options(frame_skip) - when true Peanut-GB renders only every other frame (saves CPU + bandwidth)
static mp_obj_t gbe_options(mp_obj_t self_in, mp_obj_t frame_skip) {
    gbe_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->gb.direct.frame_skip = mp_obj_is_true(frame_skip) ? 1 : 0;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(gbe_options_obj, gbe_options);

// frame(buttons, draw=True)
static mp_obj_t gbe_frame(size_t n_args, const mp_obj_t *args) {
    gbe_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    uint32_t m = (uint32_t)mp_obj_get_int(args[1]);
    self->draw_on = n_args < 3 ? true : mp_obj_is_true(args[2]);

    uint8_t j = 0xFF;  // active low
    if (m & BT_UP) j &= ~0x40;
    if (m & BT_DOWN) j &= ~0x80;
    if (m & BT_LEFT) j &= ~0x20;
    if (m & BT_RIGHT) j &= ~0x10;
    if (m & BT_A) j &= ~0x01;
    if (m & BT_B) j &= ~0x02;
    if (m & BT_START) j &= ~0x08;
    if (m & BT_SELECT) j &= ~0x04;
    self->gb.direct.joypad = j;

    self->pending = 0;
    gb_run_frame(&self->gb);
    gbe_flush(self);
    if (self->hw && self->st_strips) {
        self->st_drawn++;
    }
    self->st_strips = 0;
    // the queue keeps draining in the background; call sync() before touching the display from Python
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gbe_frame_obj, 2, 3, gbe_frame);

// sync() - wait for any running display transfer (call before drawing with Python)
static mp_obj_t gbe_sync(mp_obj_t self_in) {
    gbe_obj_t *self = MP_OBJ_TO_PTR(self_in);
    gbe_dma_wait(self);
    self->pending = 0;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(gbe_sync_obj, gbe_sync);

// stats() -> (rom bank loads, microseconds spent loading, microseconds waiting for the display)
// and resets the counters.
static mp_obj_t gbe_stats(mp_obj_t self_in) {
    gbe_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_t t[5];
    t[3] = mp_obj_new_int_from_uint(self->st_drawn);
    t[4] = mp_obj_new_int_from_uint(self->hw ? spi_get_baudrate(self->spi) : 0);
    self->st_drawn = 0;
    t[0] = mp_obj_new_int_from_uint(self->st_loads);
    t[1] = mp_obj_new_int_from_uint(self->st_load_us);
    t[2] = mp_obj_new_int_from_uint(self->st_wait_us);
    self->st_loads = 0;
    self->st_load_us = 0;
    self->st_wait_us = 0;
    return mp_obj_new_tuple(5, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(gbe_stats_obj, gbe_stats);

// ram() -> memoryview of cartridge RAM (battery save) or None
static mp_obj_t gbe_ram(mp_obj_t self_in) {
    gbe_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->ram_size) {
        return mp_const_none;
    }
    return mp_obj_new_memoryview('B' | MP_OBJ_ARRAY_TYPECODE_FLAG_RW, self->ram_size, self->ram);
}
static MP_DEFINE_CONST_FUN_OBJ_1(gbe_ram_obj, gbe_ram);

// dirty() -> True if cartridge RAM changed since the last call
static mp_obj_t gbe_dirty(mp_obj_t self_in) {
    gbe_obj_t *self = MP_OBJ_TO_PTR(self_in);
    bool d = self->dirty;
    self->dirty = false;
    return mp_obj_new_bool(d);
}
static MP_DEFINE_CONST_FUN_OBJ_1(gbe_dirty_obj, gbe_dirty);

// title() -> game title from the ROM header
static mp_obj_t gbe_title(mp_obj_t self_in) {
    gbe_obj_t *self = MP_OBJ_TO_PTR(self_in);
    char buf[17];
    int n = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t c = self->bank0[0x134 + i];
        if (c < 32 || c > 126) {
            break;
        }
        buf[n++] = (char)c;
    }
    return mp_obj_new_str(buf, n);
}
static MP_DEFINE_CONST_FUN_OBJ_1(gbe_title_obj, gbe_title);

static const mp_rom_map_elem_t gbe_locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_sd_direct), MP_ROM_PTR(&gbe_sd_direct_obj)},
    {MP_ROM_QSTR(MP_QSTR_display), MP_ROM_PTR(&gbe_display_obj)},
    {MP_ROM_QSTR(MP_QSTR_display_hw), MP_ROM_PTR(&gbe_display_hw_obj)},
    {MP_ROM_QSTR(MP_QSTR_palette), MP_ROM_PTR(&gbe_palette_obj)},
    {MP_ROM_QSTR(MP_QSTR_options), MP_ROM_PTR(&gbe_options_obj)},
    {MP_ROM_QSTR(MP_QSTR_frame), MP_ROM_PTR(&gbe_frame_obj)},
    {MP_ROM_QSTR(MP_QSTR_sync), MP_ROM_PTR(&gbe_sync_obj)},
    {MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&gbe_stats_obj)},
    {MP_ROM_QSTR(MP_QSTR_ram), MP_ROM_PTR(&gbe_ram_obj)},
    {MP_ROM_QSTR(MP_QSTR_dirty), MP_ROM_PTR(&gbe_dirty_obj)},
    {MP_ROM_QSTR(MP_QSTR_title), MP_ROM_PTR(&gbe_title_obj)},
};
static MP_DEFINE_CONST_DICT(gbe_locals_dict, gbe_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    gbe_type,
    MP_QSTR_Emu,
    MP_TYPE_FLAG_NONE,
    make_new, gbe_make_new,
    locals_dict, &gbe_locals_dict
    );

static const mp_rom_map_elem_t gbemu_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_gbemu)},
    {MP_ROM_QSTR(MP_QSTR_Emu), MP_ROM_PTR(&gbe_type)},
};
static MP_DEFINE_CONST_DICT(gbemu_globals, gbemu_globals_table);

const mp_obj_module_t gbemu_user_cmodule = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&gbemu_globals,
};

MP_REGISTER_MODULE(MP_QSTR_gbemu, gbemu_user_cmodule);
