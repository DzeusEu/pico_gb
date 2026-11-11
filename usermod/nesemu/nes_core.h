// nes_core.h - small NES (2A03 + 2C02) core for RP2040. Pure C, no MicroPython dependencies.
//
//  * 6502 with all official opcodes and the common unofficial ones (LAX, SAX, DCP, ISC, SLO, RLA, SRE, RRA, NOPs)
//  * scanline PPU (background, sprites, sprite 0 hit, loopy scrolling, per-scanline register effects)
//  * mappers 0 NROM, 1 MMC1, 2 UxROM, 3 CNROM, 4 MMC3 (scanline IRQ), 7 AxROM, 66 GxROM
//  * no APU (silent)
//
// ROM data is not held by the core: PRG/CHR are reached through 4 KB (PRG) and 1 KB (CHR)
// window pointers that the host fills in from its remap() callback.
#ifndef NES_CORE_H
#define NES_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifndef NES_HOT
#define NES_HOT
#endif

#define NES_NOMAP 0xFFFFFFFFu

// bit order = order of the serial reads of $4016
enum { NES_A = 1, NES_B = 2, NES_SELECT = 4, NES_START = 8, NES_UP = 16, NES_DOWN = 32, NES_LEFT = 64, NES_RIGHT = 128 };

typedef struct nes_s nes_t;

struct nes_s {
    // ---- CPU
    uint16_t pc;
    uint8_t a, x, y, sp, p;
    bool nmi_req;
    bool irq;                 // mapper IRQ line
    int32_t stall;            // extra CPU cycles (OAM DMA)
    uint8_t ram[2048];
    uint8_t prg_ram[8192];
    uint8_t open_bus;

    // ---- PPU
    uint8_t vram[2048];
    uint8_t oam[256];
    uint8_t pal[32];
    uint8_t chr_ram[8192];
    uint8_t ctrl, mask, status, oam_addr, rbuf, fine_x, wlatch, ppu_bus;
    uint16_t v, t;
    int line;
    uint8_t *nt_ptr[4];
    uint8_t nt_map[4];
    bool s0_pending;
    int s0_dot;
    uint8_t lineidx[256 + 16];

    // ---- input
    uint8_t keys, shift;
    bool strobe;

    // ---- cartridge
    uint8_t mapper, mirror;   // mirror: 0 horizontal 1 vertical 2 single-0 3 single-1
    bool battery, chr_is_ram, ram_dirty;
    uint32_t prg_size, chr_size;   // bytes
    uint32_t prg_off[8];      // 4 KB windows at $8000.. : offset into PRG ROM (bytes)
    uint32_t chr_off[8];      // 1 KB windows at $0000.. : offset into CHR ROM, or NES_NOMAP (= CHR RAM)
    const uint8_t *prg_ptr[8];
    const uint8_t *chr_ptr[8];
    void (*remap)(nes_t *);
    void (*line_cb)(nes_t *, int y, const uint8_t *idx);
    void *user;
    bool draw;

    // ---- mapper state
    uint8_t m_shift, m_count, m_ctrl, m_chr0, m_chr1, m_prg, m_bank;
    uint8_t m3_cmd, m3_regs[8], m3_latch, m3_counter, m3_mode_prg, m3_mode_chr;
    bool m3_reload, m3_irq_en;
};

// ------------------------------------------------------------------ cartridge helpers

static inline uint32_t nes_nprg8(const nes_t *n) { return n->prg_size >> 13; }

static inline void nes_set_prg8(nes_t *n, int slot, uint32_t bank) {   // slot 0..3 (8 KB at $8000,$A000,$C000,$E000)
    uint32_t nb = nes_nprg8(n);
    uint32_t off = (nb ? bank % nb : 0) << 13;
    n->prg_off[slot * 2] = off;
    n->prg_off[slot * 2 + 1] = off + 4096;
}

static inline void nes_set_chr1(nes_t *n, int slot, uint32_t bank1k) {
    if (n->chr_is_ram) {
        n->chr_off[slot] = NES_NOMAP;
        n->chr_ptr[slot] = n->chr_ram + ((bank1k & 7) << 10);
    } else {
        uint32_t nb = n->chr_size >> 10;
        n->chr_off[slot] = (nb ? bank1k % nb : 0) << 10;
    }
}

static inline void nes_set_mirror(nes_t *n, int m) {
    static const uint8_t map[4][4] = {{0, 0, 1, 1}, {0, 1, 0, 1}, {0, 0, 0, 0}, {1, 1, 1, 1}};
    n->mirror = m;
    for (int i = 0; i < 4; i++) {
        n->nt_map[i] = map[m][i];
        n->nt_ptr[i] = n->vram + (map[m][i] << 10);
    }
}

static void nes_mapper_update(nes_t *n) {
    switch (n->mapper) {
        case 1: {
            uint8_t c = n->m_ctrl;
            switch (c & 3) {
                case 0: nes_set_mirror(n, 2); break;
                case 1: nes_set_mirror(n, 3); break;
                case 2: nes_set_mirror(n, 1); break;
                default: nes_set_mirror(n, 0); break;
            }
            uint32_t nb16 = n->prg_size >> 14;
            uint32_t pb = n->m_prg & 15;
            int mode = (c >> 2) & 3;
            if (mode < 2) {
                nes_set_prg8(n, 0, (pb & ~1u) * 2);
                nes_set_prg8(n, 1, (pb & ~1u) * 2 + 1);
                nes_set_prg8(n, 2, (pb | 1u) * 2);
                nes_set_prg8(n, 3, (pb | 1u) * 2 + 1);
            } else if (mode == 2) {
                nes_set_prg8(n, 0, 0);
                nes_set_prg8(n, 1, 1);
                nes_set_prg8(n, 2, pb * 2);
                nes_set_prg8(n, 3, pb * 2 + 1);
            } else {
                nes_set_prg8(n, 0, pb * 2);
                nes_set_prg8(n, 1, pb * 2 + 1);
                nes_set_prg8(n, 2, (nb16 - 1) * 2);
                nes_set_prg8(n, 3, (nb16 - 1) * 2 + 1);
            }
            if (c & 0x10) {
                for (int i = 0; i < 4; i++) {
                    nes_set_chr1(n, i, (uint32_t)n->m_chr0 * 4 + i);
                    nes_set_chr1(n, 4 + i, (uint32_t)n->m_chr1 * 4 + i);
                }
            } else {
                for (int i = 0; i < 8; i++) {
                    nes_set_chr1(n, i, (uint32_t)(n->m_chr0 & ~1u) * 4 + i);
                }
            }
            break;
        }
        case 4: {
            uint32_t nb8 = nes_nprg8(n);
            uint8_t *r = n->m3_regs;
            if (n->m3_mode_prg) {
                nes_set_prg8(n, 0, nb8 - 2);
                nes_set_prg8(n, 2, r[6]);
            } else {
                nes_set_prg8(n, 0, r[6]);
                nes_set_prg8(n, 2, nb8 - 2);
            }
            nes_set_prg8(n, 1, r[7]);
            nes_set_prg8(n, 3, nb8 - 1);
            int b = n->m3_mode_chr ? 4 : 0;   // where the 2 KB banks live
            int s = n->m3_mode_chr ? 0 : 4;   // where the 1 KB banks live
            nes_set_chr1(n, b + 0, r[0] & ~1u);
            nes_set_chr1(n, b + 1, r[0] | 1u);
            nes_set_chr1(n, b + 2, r[1] & ~1u);
            nes_set_chr1(n, b + 3, r[1] | 1u);
            nes_set_chr1(n, s + 0, r[2]);
            nes_set_chr1(n, s + 1, r[3]);
            nes_set_chr1(n, s + 2, r[4]);
            nes_set_chr1(n, s + 3, r[5]);
            break;
        }
        default:
            break;
    }
    if (n->remap) {
        n->remap(n);
    }
}

static void nes_mapper_write(nes_t *n, uint16_t addr, uint8_t val) {
    switch (n->mapper) {
        case 1:
            if (val & 0x80) {
                n->m_shift = 0;
                n->m_count = 0;
                n->m_ctrl |= 0x0C;
                nes_mapper_update(n);
            } else {
                n->m_shift |= (val & 1) << n->m_count;
                if (++n->m_count == 5) {
                    uint8_t v = n->m_shift;
                    switch ((addr >> 13) & 3) {
                        case 0: n->m_ctrl = v; break;
                        case 1: n->m_chr0 = v; break;
                        case 2: n->m_chr1 = v; break;
                        default: n->m_prg = v; break;
                    }
                    n->m_shift = 0;
                    n->m_count = 0;
                    nes_mapper_update(n);
                }
            }
            break;
        case 2: {
            uint32_t nb16 = n->prg_size >> 14;
            uint32_t b = (val % nb16) * 2;
            nes_set_prg8(n, 0, b);
            nes_set_prg8(n, 1, b + 1);
            if (n->remap) n->remap(n);
            break;
        }
        case 3:
            for (int i = 0; i < 8; i++) {
                nes_set_chr1(n, i, (uint32_t)(val & 3) * 8 + i);
            }
            if (n->remap) n->remap(n);
            break;
        case 4:
            switch (((addr >> 12) & 6) | (addr & 1)) {
                case 0: n->m3_cmd = val & 7; n->m3_mode_prg = (val >> 6) & 1; n->m3_mode_chr = (val >> 7) & 1; nes_mapper_update(n); break;
                case 1: n->m3_regs[n->m3_cmd] = val; nes_mapper_update(n); break;
                case 2: nes_set_mirror(n, (val & 1) ? 0 : 1); break;
                case 3: break;                                   // PRG RAM protect
                case 4: n->m3_latch = val; break;
                case 5: n->m3_counter = 0; n->m3_reload = true; break;
                case 6: n->m3_irq_en = false; n->irq = false; break;
                default: n->m3_irq_en = true; break;
            }
            break;
        case 7: {
            uint32_t b = (val & 7) * 4;
            nes_set_prg8(n, 0, b);
            nes_set_prg8(n, 1, b + 1);
            nes_set_prg8(n, 2, b + 2);
            nes_set_prg8(n, 3, b + 3);
            nes_set_mirror(n, (val & 0x10) ? 3 : 2);
            if (n->remap) n->remap(n);
            break;
        }
        case 66: {
            uint32_t b = ((val >> 4) & 3) * 4;
            nes_set_prg8(n, 0, b);
            nes_set_prg8(n, 1, b + 1);
            nes_set_prg8(n, 2, b + 2);
            nes_set_prg8(n, 3, b + 3);
            for (int i = 0; i < 8; i++) {
                nes_set_chr1(n, i, (uint32_t)(val & 3) * 8 + i);
            }
            if (n->remap) n->remap(n);
            break;
        }
        default:
            break;
    }
}

static const uint8_t nes_supported[] = {0, 1, 2, 3, 4, 7, 66};

static bool nes_mapper_supported(int m) {
    for (unsigned i = 0; i < sizeof nes_supported; i++) {
        if (nes_supported[i] == m) {
            return true;
        }
    }
    return false;
}

// Parse a 16 byte iNES header. Returns 0 on success, else an error code:
// 1 not iNES, 2 unsupported mapper (n->mapper holds it)
static int nes_load_header(nes_t *n, const uint8_t *h, uint32_t *trainer_bytes) {
    memset(n, 0, sizeof *n);
    if (h[0] != 'N' || h[1] != 'E' || h[2] != 'S' || h[3] != 0x1A) {
        return 1;
    }
    n->prg_size = (uint32_t)h[4] * 16384;
    n->chr_size = (uint32_t)h[5] * 8192;
    n->mapper = (h[6] >> 4) | (h[7] & 0xF0);
    n->battery = (h[6] & 2) != 0;
    n->chr_is_ram = (n->chr_size == 0);
    n->mirror = (h[6] & 1) ? 1 : 0;
    *trainer_bytes = (h[6] & 4) ? 512 : 0;
    if (!nes_mapper_supported(n->mapper) || n->prg_size == 0) {
        return 2;
    }
    return 0;
}

// ------------------------------------------------------------------ PPU bus

static NES_HOT inline uint8_t nes_ppu_rd(nes_t *n, uint16_t a) {
    a &= 0x3FFF;
    if (a < 0x2000) {
        return n->chr_ptr[a >> 10][a & 0x3FF];
    }
    if (a < 0x3F00) {
        return n->nt_ptr[(a >> 10) & 3][a & 0x3FF];
    }
    a &= 0x1F;
    if ((a & 0x13) == 0x10) {
        a &= ~0x10;
    }
    return n->pal[a];
}

static NES_HOT void nes_ppu_wr(nes_t *n, uint16_t a, uint8_t val) {
    a &= 0x3FFF;
    if (a < 0x2000) {
        if (n->chr_is_ram) {
            n->chr_ram[a] = val;
        }
    } else if (a < 0x3F00) {
        n->nt_ptr[(a >> 10) & 3][a & 0x3FF] = val;
    } else {
        a &= 0x1F;
        if ((a & 0x13) == 0x10) {
            a &= ~0x10;
        }
        n->pal[a] = val & 0x3F;
    }
}

// ------------------------------------------------------------------ CPU bus

static NES_HOT uint8_t nes_rd(nes_t *n, uint16_t a) {
    if (a < 0x2000) {
        return n->ram[a & 0x7FF];
    }
    if (a >= 0x8000) {
        return n->prg_ptr[(a >> 12) & 7][a & 0xFFF];
    }
    if (a < 0x4000) {
        switch (a & 7) {
            case 2: {
                uint8_t s = (n->status & 0xE0) | (n->ppu_bus & 0x1F);
                n->status &= ~0x80;
                n->wlatch = 0;
                return s;
            }
            case 4:
                return n->oam[n->oam_addr];
            case 7: {
                uint8_t r;
                if ((n->v & 0x3FFF) >= 0x3F00) {
                    r = nes_ppu_rd(n, n->v);
                    n->rbuf = nes_ppu_rd(n, n->v - 0x1000);
                } else {
                    r = n->rbuf;
                    n->rbuf = nes_ppu_rd(n, n->v);
                }
                n->v += (n->ctrl & 4) ? 32 : 1;
                return r;
            }
            default:
                return n->ppu_bus;
        }
    }
    if (a == 0x4016 || a == 0x4017) {
        if (a == 0x4017) {
            return 0x40;
        }
        uint8_t r;
        if (n->strobe) {
            r = n->keys & 1;       // A button while strobing
        } else {
            r = n->shift & 1;
            n->shift = (n->shift >> 1) | 0x80;
        }
        return 0x40 | r;
    }
    if (a >= 0x6000) {
        return n->prg_ram[a - 0x6000];
    }
    return 0;
}

static NES_HOT void nes_wr(nes_t *n, uint16_t a, uint8_t val) {
    if (a < 0x2000) {
        n->ram[a & 0x7FF] = val;
    } else if (a >= 0x8000) {
        nes_mapper_write(n, a, val);
    } else if (a < 0x4000) {
        n->ppu_bus = val;
        switch (a & 7) {
            case 0: {
                uint8_t old = n->ctrl;
                n->ctrl = val;
                n->t = (n->t & ~0x0C00) | ((val & 3) << 10);
                if ((val & 0x80) && !(old & 0x80) && (n->status & 0x80)) {
                    n->nmi_req = true;
                }
                break;
            }
            case 1: n->mask = val; break;
            case 3: n->oam_addr = val; break;
            case 4: n->oam[n->oam_addr++] = val; break;
            case 5:
                if (!n->wlatch) {
                    n->t = (n->t & ~0x1F) | (val >> 3);
                    n->fine_x = val & 7;
                } else {
                    n->t = (n->t & ~0x73E0) | ((val & 7) << 12) | ((val >> 3) << 5);
                }
                n->wlatch ^= 1;
                break;
            case 6:
                if (!n->wlatch) {
                    n->t = (n->t & 0x00FF) | ((val & 0x3F) << 8);
                } else {
                    n->t = (n->t & 0x7F00) | val;
                    n->v = n->t;
                }
                n->wlatch ^= 1;
                break;
            case 7:
                nes_ppu_wr(n, n->v, val);
                n->v += (n->ctrl & 4) ? 32 : 1;
                break;
        }
    } else if (a == 0x4014) {
        uint16_t base = (uint16_t)val << 8;
        for (int i = 0; i < 256; i++) {
            n->oam[(n->oam_addr + i) & 0xFF] = nes_rd(n, base + i);
        }
        n->stall += 513;
    } else if (a == 0x4016) {
        bool s = val & 1;
        if (n->strobe && !s) {
            n->shift = n->keys;
        }
        n->strobe = s;
        if (s) {
            n->shift = n->keys;
        }
    } else if (a >= 0x6000) {
        if (n->prg_ram[a - 0x6000] != val) {
            n->prg_ram[a - 0x6000] = val;
            n->ram_dirty = true;
        }
    }
}

// ------------------------------------------------------------------ CPU
#include "nes_tables.h"

#define F_C 0x01
#define F_Z 0x02
#define F_I 0x04
#define F_D 0x08
#define F_B 0x10
#define F_U 0x20
#define F_V 0x40
#define F_N 0x80

#define SETNZ(v) (n->p = (n->p & ~(F_N | F_Z)) | ((v) & 0x80) | (((v) & 0xFF) == 0 ? F_Z : 0))

static NES_HOT inline void nes_push(nes_t *n, uint8_t v) {
    n->ram[0x100 + n->sp] = v;
    n->sp--;
}

static NES_HOT inline uint8_t nes_pull(nes_t *n) {
    n->sp++;
    return n->ram[0x100 + n->sp];
}

static NES_HOT inline void nes_adc(nes_t *n, uint8_t m) {
    unsigned sum = n->a + m + (n->p & F_C);
    n->p &= ~(F_C | F_V);
    if (sum > 0xFF) {
        n->p |= F_C;
    }
    if (~(n->a ^ m) & (n->a ^ sum) & 0x80) {
        n->p |= F_V;
    }
    n->a = (uint8_t)sum;
    SETNZ(n->a);
}

static NES_HOT inline void nes_cmp(nes_t *n, uint8_t r, uint8_t m) {
    unsigned t = (unsigned)r - m;
    n->p = (n->p & ~(F_C | F_N | F_Z)) | (r >= m ? F_C : 0) | (t & 0x80) | ((t & 0xFF) == 0 ? F_Z : 0);
}

static NES_HOT inline uint8_t nes_asl(nes_t *n, uint8_t v) {
    n->p = (n->p & ~F_C) | (v >> 7);
    v <<= 1;
    SETNZ(v);
    return v;
}

static NES_HOT inline uint8_t nes_lsr(nes_t *n, uint8_t v) {
    n->p = (n->p & ~F_C) | (v & 1);
    v >>= 1;
    SETNZ(v);
    return v;
}

static NES_HOT inline uint8_t nes_rol(nes_t *n, uint8_t v) {
    uint8_t c = n->p & F_C;
    n->p = (n->p & ~F_C) | (v >> 7);
    v = (v << 1) | c;
    SETNZ(v);
    return v;
}

static NES_HOT inline uint8_t nes_ror(nes_t *n, uint8_t v) {
    uint8_t c = n->p & F_C;
    n->p = (n->p & ~F_C) | (v & 1);
    v = (v >> 1) | (c << 7);
    SETNZ(v);
    return v;
}

static NES_HOT void nes_interrupt(nes_t *n, uint16_t vec, bool brk) {
    nes_push(n, n->pc >> 8);
    nes_push(n, n->pc & 0xFF);
    nes_push(n, (uint8_t)(((n->p | F_U) & ~F_B) | (brk ? F_B : 0)));
    n->p |= F_I;
    n->pc = nes_rd(n, vec) | (nes_rd(n, vec + 1) << 8);
}

// Executes one instruction (or interrupt entry); returns the CPU cycles used.
static NES_HOT int nes_cpu_step(nes_t *n) {
    if (n->nmi_req) {
        n->nmi_req = false;
        nes_interrupt(n, 0xFFFA, false);
        return 7;
    }
    if (n->irq && !(n->p & F_I)) {
        nes_interrupt(n, 0xFFFE, false);
        return 7;
    }
    uint8_t op = nes_rd(n, n->pc++);
    int cyc = nes_op_cyc[op];
    unsigned ea = 0;
    bool cross = false;
    uint8_t mode = nes_op_mode[op];
    switch (mode) {
        case M_IMM: ea = n->pc++; break;
        case M_ZP: ea = nes_rd(n, n->pc++); break;
        case M_ZPX: ea = (nes_rd(n, n->pc++) + n->x) & 0xFF; break;
        case M_ZPY: ea = (nes_rd(n, n->pc++) + n->y) & 0xFF; break;
        case M_ABS: ea = nes_rd(n, n->pc) | (nes_rd(n, n->pc + 1) << 8); n->pc += 2; break;
        case M_ABX: {
            unsigned b = nes_rd(n, n->pc) | (nes_rd(n, n->pc + 1) << 8);
            n->pc += 2;
            ea = (b + n->x) & 0xFFFF;
            cross = (b & 0xFF00) != (ea & 0xFF00);
            break;
        }
        case M_ABY: {
            unsigned b = nes_rd(n, n->pc) | (nes_rd(n, n->pc + 1) << 8);
            n->pc += 2;
            ea = (b + n->y) & 0xFFFF;
            cross = (b & 0xFF00) != (ea & 0xFF00);
            break;
        }
        case M_IZX: {
            unsigned z = (nes_rd(n, n->pc++) + n->x) & 0xFF;
            ea = n->ram[z] | (n->ram[(z + 1) & 0xFF] << 8);
            break;
        }
        case M_IZY: {
            unsigned z = nes_rd(n, n->pc++);
            unsigned b = n->ram[z] | (n->ram[(z + 1) & 0xFF] << 8);
            ea = (b + n->y) & 0xFFFF;
            cross = (b & 0xFF00) != (ea & 0xFF00);
            break;
        }
        case M_IND: {
            unsigned p = nes_rd(n, n->pc) | (nes_rd(n, n->pc + 1) << 8);
            n->pc += 2;
            ea = nes_rd(n, p) | (nes_rd(n, (p & 0xFF00) | ((p + 1) & 0xFF)) << 8);   // page wrap bug
            break;
        }
        default:
            break;
    }
    if (cross && nes_op_pg[op]) {
        cyc++;
    }
    uint8_t m;
    switch (nes_op_ins[op]) {
        case I_ADC: nes_adc(n, nes_rd(n, ea)); break;
        case I_SBC: nes_adc(n, ~nes_rd(n, ea)); break;
        case I_AND: n->a &= nes_rd(n, ea); SETNZ(n->a); break;
        case I_ORA: n->a |= nes_rd(n, ea); SETNZ(n->a); break;
        case I_EOR: n->a ^= nes_rd(n, ea); SETNZ(n->a); break;
        case I_CMP: nes_cmp(n, n->a, nes_rd(n, ea)); break;
        case I_CPX: nes_cmp(n, n->x, nes_rd(n, ea)); break;
        case I_CPY: nes_cmp(n, n->y, nes_rd(n, ea)); break;
        case I_LDA: n->a = nes_rd(n, ea); SETNZ(n->a); break;
        case I_LDX: n->x = nes_rd(n, ea); SETNZ(n->x); break;
        case I_LDY: n->y = nes_rd(n, ea); SETNZ(n->y); break;
        case I_STA: nes_wr(n, ea, n->a); break;
        case I_STX: nes_wr(n, ea, n->x); break;
        case I_STY: nes_wr(n, ea, n->y); break;
        case I_BIT:
            m = nes_rd(n, ea);
            n->p = (n->p & ~(F_N | F_V | F_Z)) | (m & 0xC0) | ((n->a & m) == 0 ? F_Z : 0);
            break;
        case I_ASL:
            if (mode == M_ACC) n->a = nes_asl(n, n->a); else nes_wr(n, ea, nes_asl(n, nes_rd(n, ea)));
            break;
        case I_LSR:
            if (mode == M_ACC) n->a = nes_lsr(n, n->a); else nes_wr(n, ea, nes_lsr(n, nes_rd(n, ea)));
            break;
        case I_ROL:
            if (mode == M_ACC) n->a = nes_rol(n, n->a); else nes_wr(n, ea, nes_rol(n, nes_rd(n, ea)));
            break;
        case I_ROR:
            if (mode == M_ACC) n->a = nes_ror(n, n->a); else nes_wr(n, ea, nes_ror(n, nes_rd(n, ea)));
            break;
        case I_INC: m = nes_rd(n, ea) + 1; nes_wr(n, ea, m); SETNZ(m); break;
        case I_DEC: m = nes_rd(n, ea) - 1; nes_wr(n, ea, m); SETNZ(m); break;
        case I_INX: n->x++; SETNZ(n->x); break;
        case I_INY: n->y++; SETNZ(n->y); break;
        case I_DEX: n->x--; SETNZ(n->x); break;
        case I_DEY: n->y--; SETNZ(n->y); break;
        case I_TAX: n->x = n->a; SETNZ(n->x); break;
        case I_TAY: n->y = n->a; SETNZ(n->y); break;
        case I_TXA: n->a = n->x; SETNZ(n->a); break;
        case I_TYA: n->a = n->y; SETNZ(n->a); break;
        case I_TSX: n->x = n->sp; SETNZ(n->x); break;
        case I_TXS: n->sp = n->x; break;
        case I_PHA: nes_push(n, n->a); break;
        case I_PHP: nes_push(n, n->p | F_B | F_U); break;
        case I_PLA: n->a = nes_pull(n); SETNZ(n->a); break;
        case I_PLP: n->p = (nes_pull(n) & ~F_B) | F_U; break;
        case I_CLC: n->p &= ~F_C; break;
        case I_SEC: n->p |= F_C; break;
        case I_CLI: n->p &= ~F_I; break;
        case I_SEI: n->p |= F_I; break;
        case I_CLD: n->p &= ~F_D; break;
        case I_SED: n->p |= F_D; break;
        case I_CLV: n->p &= ~F_V; break;
        case I_JMP: n->pc = ea; break;
        case I_JSR: {
            unsigned ret = n->pc - 1;     // ea already fetched; push address of last operand byte
            nes_push(n, ret >> 8);
            nes_push(n, ret & 0xFF);
            n->pc = ea;
            break;
        }
        case I_RTS: {
            unsigned lo = nes_pull(n);
            unsigned hi = nes_pull(n);
            n->pc = ((hi << 8) | lo) + 1;
            break;
        }
        case I_RTI: {
            n->p = (nes_pull(n) & ~F_B) | F_U;
            unsigned lo = nes_pull(n);
            unsigned hi = nes_pull(n);
            n->pc = (hi << 8) | lo;
            break;
        }
        case I_BRK:
            n->pc++;
            nes_interrupt(n, 0xFFFE, true);
            break;
#define BRANCH(cond) \
    { int8_t off = (int8_t)nes_rd(n, n->pc++); \
      if (cond) { unsigned np = n->pc + off; cyc += 1 + (((np ^ n->pc) & 0xFF00) ? 1 : 0); n->pc = np; } }
        case I_BPL: BRANCH(!(n->p & F_N)) break;
        case I_BMI: BRANCH(n->p & F_N) break;
        case I_BVC: BRANCH(!(n->p & F_V)) break;
        case I_BVS: BRANCH(n->p & F_V) break;
        case I_BCC: BRANCH(!(n->p & F_C)) break;
        case I_BCS: BRANCH(n->p & F_C) break;
        case I_BNE: BRANCH(!(n->p & F_Z)) break;
        case I_BEQ: BRANCH(n->p & F_Z) break;
        // ---- unofficial
        case I_NOP:
            if (mode != M_IMP && mode != M_ACC && mode != M_IMM) {
                (void)nes_rd(n, ea);
            }
            break;
        case I_LAX: n->a = n->x = nes_rd(n, ea); SETNZ(n->a); break;
        case I_SAX: nes_wr(n, ea, n->a & n->x); break;
        case I_DCP: m = nes_rd(n, ea) - 1; nes_wr(n, ea, m); nes_cmp(n, n->a, m); break;
        case I_ISC: m = nes_rd(n, ea) + 1; nes_wr(n, ea, m); nes_adc(n, ~m); break;
        case I_SLO: m = nes_asl(n, nes_rd(n, ea)); nes_wr(n, ea, m); n->a |= m; SETNZ(n->a); break;
        case I_RLA: m = nes_rol(n, nes_rd(n, ea)); nes_wr(n, ea, m); n->a &= m; SETNZ(n->a); break;
        case I_SRE: m = nes_lsr(n, nes_rd(n, ea)); nes_wr(n, ea, m); n->a ^= m; SETNZ(n->a); break;
        case I_RRA: m = nes_ror(n, nes_rd(n, ea)); nes_wr(n, ea, m); nes_adc(n, m); break;
        default:
            break;
    }
    return cyc;
}

// ------------------------------------------------------------------ PPU rendering

static uint16_t nes_spread[256];    // bit-plane spreading table (bits -> 2 bits per pixel positions)
static bool nes_spread_ready = false;

static void nes_init_tables(void) {
    if (nes_spread_ready) {
        return;
    }
    for (int i = 0; i < 256; i++) {
        uint16_t r = 0;
        for (int b = 0; b < 8; b++) {
            if (i & (0x80 >> b)) {
                r |= 1 << (14 - 2 * b);      // pixel b occupies bits (14-2b, 15-2b)
            }
        }
        nes_spread[i] = r;
    }
    nes_spread_ready = true;
}

// Renders scanline y into n->lineidx (palette indices 0..63). Also finds the sprite 0 hit position.
static NES_HOT void nes_render_line(nes_t *n, int y, bool output) {
    uint8_t *out = n->lineidx;
    uint8_t bgbuf[256 + 16];           // 2-bit colour in bits 0-1, palette in bits 2-3
    uint8_t sprbuf[256];               // bit7 = opaque, bit6 = behind bg, low 5 bits = palette index (16..31)
    bool bg_on = (n->mask & 0x08) != 0;
    bool spr_on = (n->mask & 0x10) != 0;
    int hit_x = -1;

    if (bg_on) {
        unsigned v = n->v;
        unsigned fine_y = (v >> 12) & 7;
        unsigned cx = v & 31;
        unsigned cy = (v >> 5) & 31;
        unsigned nt = (v >> 10) & 3;
        unsigned base = (n->ctrl & 0x10) ? 0x1000 : 0;
        uint8_t *dst = bgbuf;
        for (int tx = 0; tx < 33; tx++) {
            const uint8_t *ntp = n->nt_ptr[nt];
            unsigned tile = ntp[(cy << 5) | cx];
            unsigned at = ntp[0x3C0 | ((cy >> 2) << 3) | (cx >> 2)];
            unsigned pal = (at >> (((cy & 2) << 1) | (cx & 2))) & 3;
            unsigned pa = base + tile * 16 + fine_y;
            const uint8_t *cp = n->chr_ptr[pa >> 10];
            unsigned lo = cp[pa & 0x3FF];
            unsigned hi = cp[(pa & 0x3FF) + 8];
            unsigned bits = nes_spread[lo] | (nes_spread[hi] << 1);
            unsigned pb = pal << 2;
            dst[0] = ((bits >> 14) & 3) ? (((bits >> 14) & 3) | pb) : 0;
            dst[1] = ((bits >> 12) & 3) ? (((bits >> 12) & 3) | pb) : 0;
            dst[2] = ((bits >> 10) & 3) ? (((bits >> 10) & 3) | pb) : 0;
            dst[3] = ((bits >> 8) & 3) ? (((bits >> 8) & 3) | pb) : 0;
            dst[4] = ((bits >> 6) & 3) ? (((bits >> 6) & 3) | pb) : 0;
            dst[5] = ((bits >> 4) & 3) ? (((bits >> 4) & 3) | pb) : 0;
            dst[6] = ((bits >> 2) & 3) ? (((bits >> 2) & 3) | pb) : 0;
            dst[7] = (bits & 3) ? ((bits & 3) | pb) : 0;
            dst += 8;
            if (++cx == 32) {
                cx = 0;
                nt ^= 1;
            }
        }
        if (!(n->mask & 0x02)) {
            for (int i = 0; i < 8; i++) {
                bgbuf[n->fine_x + i] = 0;
            }
        }
    } else {
        memset(bgbuf, 0, sizeof bgbuf);
    }

    if (spr_on) {
        memset(sprbuf, 0, sizeof sprbuf);
        int h = (n->ctrl & 0x20) ? 16 : 8;
        int found = 0;
        uint8_t s0mask[256 / 8 + 1];
        bool s0_here = false;
        int idx_list[8];
        for (int i = 0; i < 64; i++) {
            int sy = n->oam[i * 4] + 1;
            int row = y - sy;
            if (row >= 0 && row < h) {
                if (found < 8) {
                    idx_list[found++] = i;
                } else {
                    n->status |= 0x20;
                    break;
                }
            }
        }
        memset(s0mask, 0, sizeof s0mask);
        for (int k = found - 1; k >= 0; k--) {
            int i = idx_list[k];
            int sy = n->oam[i * 4] + 1;
            unsigned tile = n->oam[i * 4 + 1];
            unsigned attr = n->oam[i * 4 + 2];
            int sx = n->oam[i * 4 + 3];
            int row = y - sy;
            if (attr & 0x80) {
                row = h - 1 - row;
            }
            unsigned pa;
            if (h == 16) {
                pa = ((tile & 1) ? 0x1000 : 0) + (tile & 0xFE) * 16 + (row & 8 ? 16 : 0) + (row & 7);
            } else {
                pa = ((n->ctrl & 0x08) ? 0x1000 : 0) + tile * 16 + row;
            }
            const uint8_t *cp = n->chr_ptr[pa >> 10];
            unsigned lo = cp[pa & 0x3FF];
            unsigned hi = cp[(pa & 0x3FF) + 8];
            unsigned bits = nes_spread[lo] | (nes_spread[hi] << 1);
            unsigned palbase = 16 + ((attr & 3) << 2);
            for (int px = 0; px < 8; px++) {
                int sh = (attr & 0x40) ? (2 * px) : (14 - 2 * px);
                unsigned c = (bits >> sh) & 3;
                if (!c) {
                    continue;
                }
                int x = sx + px;
                if (x > 255) {
                    break;
                }
                sprbuf[x] = 0x80 | ((attr & 0x20) ? 0x40 : 0) | (palbase + c);
                if (i == 0) {
                    s0mask[x >> 3] |= 1 << (x & 7);
                    s0_here = true;
                }
            }
        }
        if (s0_here && !(n->status & 0x40)) {
            for (int x = 0; x < 255; x++) {
                if (!(s0mask[x >> 3] & (1 << (x & 7)))) {
                    continue;
                }
                if (x < 8 && (!(n->mask & 0x02) || !(n->mask & 0x04))) {
                    continue;
                }
                if (bgbuf[n->fine_x + x] & 3) {
                    hit_x = x;
                    break;
                }
            }
        }
        if (!(n->mask & 0x04)) {
            for (int x = 0; x < 8; x++) {
                sprbuf[x] = 0;
            }
        }
    }
    if (hit_x >= 0) {
        n->s0_pending = true;
        n->s0_dot = hit_x + 4;
    }
    if (!output) {
        return;
    }

    const uint8_t *pal = n->pal;
    uint8_t grey = (n->mask & 1) ? 0x30 : 0x3F;
    uint8_t back = pal[0] & grey;
    if (!bg_on && !spr_on) {
        memset(out, back, 256);
        return;
    }
    const uint8_t *bgp = bgbuf + n->fine_x;
    if (spr_on) {
        for (int x = 0; x < 256; x++) {
            unsigned b = bgp[x];
            unsigned s = sprbuf[x];
            if (s && (!(s & 0x40) || !(b & 3))) {
                out[x] = pal[s & 0x1F] & grey;
            } else {
                out[x] = b ? (pal[b] & grey) : back;
            }
        }
    } else {
        for (int x = 0; x < 256; x++) {
            unsigned b = bgp[x];
            out[x] = b ? (pal[b] & grey) : back;
        }
    }
}

static NES_HOT void nes_end_line_scroll(nes_t *n) {
    unsigned v = n->v;
    if ((v & 0x7000) != 0x7000) {
        v += 0x1000;
    } else {
        v &= ~0x7000u;
        unsigned y = (v & 0x3E0) >> 5;
        if (y == 29) {
            y = 0;
            v ^= 0x800;
        } else if (y == 31) {
            y = 0;
        } else {
            y++;
        }
        v = (v & ~0x3E0u) | (y << 5);
    }
    v = (v & ~0x41Fu) | (n->t & 0x41F);
    n->v = v;
}

// ------------------------------------------------------------------ frame

static void nes_reset(nes_t *n) {
    n->a = n->x = n->y = 0;
    n->sp = 0xFD;
    n->p = F_I | F_U;
    n->nmi_req = false;
    n->irq = false;
    n->stall = 0;
    n->ctrl = n->mask = n->status = 0;
    n->v = n->t = 0;
    n->wlatch = 0;
    n->fine_x = 0;
    n->line = 0;
    n->pc = nes_rd(n, 0xFFFC) | (nes_rd(n, 0xFFFD) << 8);
}

// Initialise mapper state after nes_load_header(). The host must have set n->remap first.
static void nes_cart_init(nes_t *n) {
    nes_init_tables();
    static const uint8_t initpal[32] = {0x09, 0x01, 0x00, 0x01, 0x00, 0x02, 0x02, 0x0D, 0x08, 0x10, 0x08, 0x24, 0x00, 0x00, 0x04, 0x2C,
                                        0x09, 0x01, 0x34, 0x03, 0x00, 0x04, 0x00, 0x14, 0x08, 0x3A, 0x00, 0x02, 0x00, 0x20, 0x2C, 0x08};
    memcpy(n->pal, initpal, 32);
    nes_set_mirror(n, n->mirror);
    uint32_t nb8 = nes_nprg8(n);
    for (int i = 0; i < 8; i++) {
        n->chr_ptr[i] = n->chr_ram + i * 1024;
        n->chr_off[i] = NES_NOMAP;
        n->prg_ptr[i] = n->prg_ram;
    }
    n->m_ctrl = 0x0C;
    n->m_prg = 0;
    switch (n->mapper) {
        case 1:
            nes_mapper_update(n);
            break;
        case 4:
            n->m3_regs[0] = 0; n->m3_regs[1] = 2; n->m3_regs[2] = 4; n->m3_regs[3] = 5; n->m3_regs[4] = 6; n->m3_regs[5] = 7;
            n->m3_regs[6] = 0; n->m3_regs[7] = 1;
            nes_mapper_update(n);
            break;
        case 2:
            nes_set_prg8(n, 0, 0); nes_set_prg8(n, 1, 1);
            nes_set_prg8(n, 2, nb8 - 2); nes_set_prg8(n, 3, nb8 - 1);
            for (int i = 0; i < 8; i++) nes_set_chr1(n, i, i);
            if (n->remap) n->remap(n);
            break;
        default:
            if (n->prg_size == 16384) {   // NROM-128 mirrors
                nes_set_prg8(n, 0, 0); nes_set_prg8(n, 1, 1); nes_set_prg8(n, 2, 0); nes_set_prg8(n, 3, 1);
            } else {
                for (uint32_t i = 0; i < 4; i++) nes_set_prg8(n, i, i);
            }
            for (int i = 0; i < 8; i++) nes_set_chr1(n, i, i);
            if (n->remap) n->remap(n);
            break;
    }
    nes_reset(n);
}

static NES_HOT void nes_run_cpu(nes_t *n, int dots, int *carry_dots, int hit_dot, bool *hit_done) {
    int budget = dots + *carry_dots;
    int used = 0;
    while (used < budget) {
        if (n->stall > 0) {
            int s = n->stall > 8 ? 8 : n->stall;
            n->stall -= s;
            used += s * 3;
        } else {
            used += nes_cpu_step(n) * 3;
        }
        if (!*hit_done && hit_dot >= 0 && used >= hit_dot) {
            n->status |= 0x40;
            *hit_done = true;
        }
    }
    *carry_dots = budget - used;
}

// Run one frame (262 scanlines). keys: NES_* bits. draw: also output pixels via line_cb.
static NES_HOT void nes_frame(nes_t *n, uint8_t keys, bool draw) {
    n->keys = keys;
    int carry = 0;
    for (int line = 0; line < 262; line++) {
        n->line = line;
        bool rendering = (n->mask & 0x18) != 0;
        int hit_dot = -1;
        bool hit_done = false;
        if (line == 241) {
            n->status |= 0x80;
            if (n->ctrl & 0x80) {
                n->nmi_req = true;
            }
        } else if (line == 261) {
            n->status &= ~(0xE0);
        }
        n->s0_pending = false;
        if (line < 240) {
            bool need_s0 = !(n->status & 0x40) && (n->mask & 0x10);
            if (draw || (need_s0 && rendering)) {
                nes_render_line(n, line, draw);
                if (draw && n->line_cb) {
                    n->line_cb(n, line, n->lineidx);
                }
            }
            if (n->s0_pending) {
                hit_dot = n->s0_dot;
            }
        }
        // cycles of this line: run to dot 260, clock MMC3, then the rest
        nes_run_cpu(n, 260, &carry, hit_dot, &hit_done);
        if (n->mapper == 4 && rendering && (line < 240 || line == 261)) {
            if (n->m3_counter == 0 || n->m3_reload) {
                n->m3_counter = n->m3_latch;
                n->m3_reload = false;
            } else {
                n->m3_counter--;
            }
            if (n->m3_counter == 0 && n->m3_irq_en) {
                n->irq = true;
            }
        }
        nes_run_cpu(n, 341 - 260, &carry, hit_dot, &hit_done);
        if (rendering && (line < 240 || line == 261)) {
            nes_end_line_scroll(n);
            if (line == 261) {
                n->v = (n->v & ~0x7BE0u) | (n->t & 0x7BE0);
            }
        }
    }
}

#endif
