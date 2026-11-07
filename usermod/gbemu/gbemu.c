// gbemu - MicroPython user C module wrapping the Peanut-GB Game Boy emulator core.
// Peanut-GB (c) Mahyar Koshkouei, MIT license: https://github.com/deltabeard/Peanut-GB
//
//   import gbemu
//   emu = gbemu.Emu(open("game.gb", "rb"), slots)   # ROM stays on the SD card
//   emu.display(lcd.blit, x, y, mode)               # mode 0 = 160x144, 1 = 240x216
//   emu.palette(c0, c1, c2, c3)                     # 4 shades, lightest first
//   emu.frame(buttons, draw)                        # run one frame (~16.7 ms of game time)
//   emu.ram() / emu.dirty() / emu.title()
//
// ROM banks are cached in RAM (bank 0 always resident, plus `slots` x 16 KB with LRU
// replacement) and fetched from the file on demand, so ROMs far bigger than RAM work.

#include <string.h>

#include "py/runtime.h"
#include "py/obj.h"
#include "py/objarray.h"
#include "py/stream.h"
#include "py/mperrno.h"

#define ENABLE_LCD 1
#define ENABLE_SOUND 0
#define PEANUT_GB_HIGH_LCD_ACCURACY 0
#include "peanut_gb.h"

#define GBE_BANK 16384
#define GBE_MAXS 8
#define GBE_OUT_LINES 12
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
    int32_t slot_bank[GBE_MAXS];
    uint32_t slot_use[GBE_MAXS];
    uint32_t tick;
    int32_t last_bank;
    uint8_t *last_ptr;
    uint8_t *ram;
    uint32_t ram_size;
    bool dirty;
    uint16_t pal[4];
    int ox, oy, mode;
    bool draw_on;
    int pending, pend_y;
    uint16_t obuf[GBE_OUT_LINES * 240];
} gbe_obj_t;

extern const mp_obj_type_t gbe_type;

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

static uint8_t *gbe_load_bank(gbe_obj_t *self, uint32_t bank) {
    int slot = -1;
    for (int i = 0; i < self->nslots; i++) {
        if (self->slot_bank[i] == (int32_t)bank) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        uint32_t oldest = 0xFFFFFFFFu;
        for (int i = 0; i < self->nslots; i++) {
            if (self->slot_bank[i] < 0) {
                slot = i;
                break;
            }
            if (self->slot_use[i] < oldest) {
                oldest = self->slot_use[i];
                slot = i;
            }
        }
        if (self->last_bank == self->slot_bank[slot]) {
            self->last_bank = -1;
        }
        self->slot_bank[slot] = -1;  // stays invalid if the read below raises
        gbe_read_at(self, bank * GBE_BANK, self->cache + (size_t)slot * GBE_BANK, GBE_BANK);
        self->slot_bank[slot] = (int32_t)bank;
    }
    self->slot_use[slot] = ++self->tick;
    self->last_bank = (int32_t)bank;
    self->last_ptr = self->cache + (size_t)slot * GBE_BANK;
    return self->last_ptr;
}

// ------------------------------------------------------------------ Peanut-GB callbacks

static uint8_t gbe_rom_read(struct gb_s *gb, const uint_fast32_t addr) {
    gbe_obj_t *self = (gbe_obj_t *)gb->direct.priv;
    if (addr < GBE_BANK) {
        return self->bank0[addr];
    }
    uint32_t bank = (uint32_t)(addr >> 14);
    if ((int32_t)bank == self->last_bank) {
        return self->last_ptr[addr & (GBE_BANK - 1)];
    }
    return gbe_load_bank(self, bank)[addr & (GBE_BANK - 1)];
}

static uint8_t gbe_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr) {
    gbe_obj_t *self = (gbe_obj_t *)gb->direct.priv;
    return addr < self->ram_size ? self->ram[addr] : 0xFF;
}

static void gbe_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val) {
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

static void gbe_flush(gbe_obj_t *self) {
    if (self->pending == 0) {
        return;
    }
    int outw = self->mode == 0 ? 160 : 240;
    mp_obj_t args[5];
    args[0] = mp_obj_new_memoryview('B', (size_t)self->pending * outw * 2, self->obuf);
    args[1] = MP_OBJ_NEW_SMALL_INT(self->ox);
    args[2] = MP_OBJ_NEW_SMALL_INT(self->oy + self->pend_y);
    args[3] = MP_OBJ_NEW_SMALL_INT(outw);
    args[4] = MP_OBJ_NEW_SMALL_INT(self->pending);
    self->pending = 0;
    mp_call_function_n_kw(self->draw, 5, 0, args);
}

static void gbe_lcd_line(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line) {
    gbe_obj_t *self = (gbe_obj_t *)gb->direct.priv;
    if (!self->draw_on || self->draw == mp_const_none) {
        return;
    }
    const uint16_t *pal = self->pal;
    if (self->mode == 0) {
        if (self->pending == 0) {
            self->pend_y = line;
        }
        uint16_t *row = self->obuf + self->pending * 160;
        for (int x = 0; x < 160; x++) {
            row[x] = pal[pixels[x] & 3];
        }
        self->pending++;
    } else {
        // 1.5x nearest neighbour: 160x144 -> 240x216
        int start = (3 * (int)line + 1) / 2;
        int count = (line & 1) ? 1 : 2;
        if (self->pending == 0) {
            self->pend_y = start;
        }
        uint16_t *row = self->obuf + self->pending * 240;
        int o = 0;
        for (int x = 0; x < 160; x += 2) {
            uint16_t c0 = pal[pixels[x] & 3];
            uint16_t c1 = pal[pixels[x + 1] & 3];
            row[o++] = c0;
            row[o++] = c0;
            row[o++] = c1;
        }
        if (count == 2) {
            memcpy(row + 240, row, 240 * sizeof(uint16_t));
        }
        self->pending += count;
    }
    if (self->pending + 2 > GBE_OUT_LINES) {
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
    enum { ARG_file, ARG_slots };
    static const mp_arg_t allowed[] = {
        {MP_QSTR_file, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_slots, MP_ARG_INT, {.u_int = 3}},
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
    self->last_bank = -1;
    self->mode = 1;
    self->pal[0] = 0xFFFF;
    self->pal[1] = 0xAAAA;
    self->pal[2] = 0x5555;
    self->pal[3] = 0x0000;
    for (int i = 0; i < GBE_MAXS; i++) {
        self->slot_bank[i] = -1;
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
    self->cache = m_new(uint8_t, (size_t)slots * GBE_BANK);
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

// display(callback, x, y, mode)
static mp_obj_t gbe_display(size_t n_args, const mp_obj_t *args) {
    gbe_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    self->draw = args[1];
    self->ox = mp_obj_get_int(args[2]);
    self->oy = mp_obj_get_int(args[3]);
    self->mode = mp_obj_get_int(args[4]) ? 1 : 0;
    self->pending = 0;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gbe_display_obj, 5, 5, gbe_display);

// palette(c0, c1, c2, c3)  - values as produced by st7789.rgb()
static mp_obj_t gbe_palette(size_t n_args, const mp_obj_t *args) {
    gbe_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    for (int i = 0; i < 4; i++) {
        self->pal[i] = (uint16_t)mp_obj_get_int(args[1 + i]);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gbe_palette_obj, 5, 5, gbe_palette);

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
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gbe_frame_obj, 2, 3, gbe_frame);

// ram() -> memoryview of cartridge RAM (battery save) or None
static mp_obj_t gbe_ram(mp_obj_t self_in) {
    gbe_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->ram_size) {
        return mp_const_none;
    }
    return mp_obj_new_memoryview('B', self->ram_size, self->ram);
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
    {MP_ROM_QSTR(MP_QSTR_display), MP_ROM_PTR(&gbe_display_obj)},
    {MP_ROM_QSTR(MP_QSTR_palette), MP_ROM_PTR(&gbe_palette_obj)},
    {MP_ROM_QSTR(MP_QSTR_frame), MP_ROM_PTR(&gbe_frame_obj)},
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
