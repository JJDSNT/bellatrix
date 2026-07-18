// src/machine/expansions/rtg/rtg.c — see rtg.h and docs/rtg_design.md.

#include "machine/expansions/rtg/rtg.h"
#include "machine/bus/board_registry.h"
#include "machine/autoconfig/autoconfig.h"
#include "machine/expansion.h"
#include "machine/machine.h"
#include "support.h"

#include <string.h>

typedef struct RtgState {
    uint8_t  vram[BELLATRIX_RTG_VRAM_SIZE];
    uint32_t palette[256];       /* 0x00RRGGBB as written by the guest */
    uint32_t enable;
    uint32_t mode_w;
    uint32_t mode_h;
    uint32_t format;
    uint32_t bytes_per_row;
    uint32_t pan;
    uint32_t pal_index;
    uint32_t vblank;
    /* rendered RGBA frame, sized lazily to mode_w*mode_h*4 */
    uint8_t  frame[1920u * 1080u * 4u];
} RtgState;

static RtgState s_rtg;
static uint8_t  s_rtg_config[AUTOCONFIG_DATA_SIZE];
static int      s_registered = 0;

/* Boot ROM: DiagArea loader (CardLoader) + relocated bellatrix.card hunk,
 * built by cards/bellatrix.card/{Makefile,bootrom/} and embedded via
 * scripts/rom_to_c.py (see tools/harness/CMakeLists.txt, rtg_rom_data). */
extern const unsigned char g_rtg_rom_data[];
extern const size_t        g_rtg_rom_size;

/* ----------------------------------------------------------------------- */
/* register file                                                            */
/* ----------------------------------------------------------------------- */

static uint32_t rtg_reg_read(uint32_t reg)
{
    if (reg == RTG_REG_ID) {
        static int s_id_read_seen = 0;
        if (!s_id_read_seen) {
            s_id_read_seen = 1;
            kprintf("[RTG] REG_ID probed (FindCard reached the board)\n");
        }
    }
    switch (reg) {
        case RTG_REG_ID:            return RTG_ID_MAGIC;
        case RTG_REG_VERSION:       return RTG_SPEC_VERSION;
        case RTG_REG_VRAM_OFF:      return BELLATRIX_RTG_VRAM_OFF;
        case RTG_REG_VRAM_SIZE:     return BELLATRIX_RTG_VRAM_SIZE;
        case RTG_REG_ENABLE:        return s_rtg.enable;
        case RTG_REG_MODE_W:        return s_rtg.mode_w;
        case RTG_REG_MODE_H:        return s_rtg.mode_h;
        case RTG_REG_FORMAT:        return s_rtg.format;
        case RTG_REG_BYTES_PER_ROW: return s_rtg.bytes_per_row;
        case RTG_REG_PAN:           return s_rtg.pan;
        case RTG_REG_PAL_INDEX:     return s_rtg.pal_index;
        case RTG_REG_VBLANK:        return s_rtg.vblank;
        default:                    return 0;
    }
}

static void rtg_reg_write(uint32_t reg, uint32_t value)
{
    {
        static uint32_t s_seen_mask;
        uint32_t bit = 1u << ((reg >> 2) & 31u);
        if (!(s_seen_mask & bit)) {
            s_seen_mask |= bit;
            kprintf("[RTG] first write reg=%02x value=%08x\n",
                    (unsigned)reg, (unsigned)value);
        }
    }
    switch (reg) {
        case RTG_REG_ENABLE:
            if (value != s_rtg.enable)
                kprintf("[RTG] enable=%u %ux%u fmt=%u bpr=%u\n",
                        (unsigned)value, (unsigned)s_rtg.mode_w,
                        (unsigned)s_rtg.mode_h, (unsigned)s_rtg.format,
                        (unsigned)s_rtg.bytes_per_row);
            s_rtg.enable = value & 1u;
            break;
        case RTG_REG_MODE_W:        s_rtg.mode_w = value;        break;
        case RTG_REG_MODE_H:        s_rtg.mode_h = value;        break;
        case RTG_REG_FORMAT:        s_rtg.format = value;        break;
        case RTG_REG_BYTES_PER_ROW: s_rtg.bytes_per_row = value; break;
        case RTG_REG_PAN:           s_rtg.pan = value;           break;
        case RTG_REG_PAL_INDEX:     s_rtg.pal_index = value & 0xFFu; break;
        case RTG_REG_PAL_DATA:
            s_rtg.palette[s_rtg.pal_index] = value & 0x00FFFFFFu;
            s_rtg.pal_index = (s_rtg.pal_index + 1u) & 0xFFu;
            break;
        case RTG_REG_DEBUG:
            kprintf("[RTG-DBG] %08x\n", (unsigned)value);
            break;
        default: break;
    }
}

/* ----------------------------------------------------------------------- */
/* Zorro II byte ops (regs are 32-bit BE, assembled from byte lanes)        */
/* ----------------------------------------------------------------------- */

/* Byte write latch so 8/16-bit accesses to the 32-bit registers work:
 * bytes accumulate and the register commits on the write of its last
 * byte (offset % 4 == 3). Reads are stateless slices of the register. */
static uint32_t s_reg_latch[16];

static uint8_t rtg_read8(void *userdata, uint32_t offset)
{
    (void)userdata;
    if (offset >= BELLATRIX_RTG_VRAM_OFF) {
        return s_rtg.vram[offset - BELLATRIX_RTG_VRAM_OFF];
    }
    if (offset >= BELLATRIX_RTG_ROM_OFF) {
        static int s_rom_read_seen = 0;
        static int s_hunk_read_seen = 0;
        uint32_t rom_off = offset - BELLATRIX_RTG_ROM_OFF;
        if (!s_rom_read_seen) {
            s_rom_read_seen = 1;
            kprintf("[RTG] first ROM read at window offset 0x%x — DiagArea probed\n",
                    (unsigned)offset);
        }
        if (offset == BELLATRIX_RTG_CARD_OFF && !s_hunk_read_seen) {
            s_hunk_read_seen = 1;
            kprintf("[RTG] card hunk read starting — CardLoader relocating bellatrix.card\n");
        }
        return (rom_off < g_rtg_rom_size) ? g_rtg_rom_data[rom_off] : 0u;
    }
    {
        uint32_t reg  = offset & ~3u;
        uint32_t v    = rtg_reg_read(reg);
        unsigned lane = offset & 3u;
        return (uint8_t)(v >> ((3u - lane) * 8u));
    }
}

static void rtg_write8(void *userdata, uint32_t offset, uint8_t value)
{
    (void)userdata;
    if (offset >= BELLATRIX_RTG_VRAM_OFF) {
        s_rtg.vram[offset - BELLATRIX_RTG_VRAM_OFF] = value;
        return;
    }
    if (offset >= BELLATRIX_RTG_ROM_OFF) {
        return;  /* ROM: read-only */
    }
    {
        uint32_t reg  = offset & ~3u;
        unsigned lane = offset & 3u;
        unsigned slot = (reg >> 2) & 15u;
        uint32_t shift = (3u - lane) * 8u;
        s_reg_latch[slot] &= ~(0xFFu << shift);
        s_reg_latch[slot] |= ((uint32_t)value << shift);
        if (lane == 3u)
            rtg_reg_write(reg, s_reg_latch[slot]);
    }
}

static void rtg_reset(void *userdata)
{
    (void)userdata;
    s_rtg.enable = 0;
    s_rtg.pan = 0;
    s_rtg.pal_index = 0;
}

/* -----------------------------------------------------------------------
 * board_registry EXTERNAL Zorro III board
 *
 * RTG is a Zorro III board: its 4MB register+ROM+VRAM window is served per
 * access (EXTERNAL, map == NULL), so the walker only latches the guest-assigned
 * base into s_rtg_board.map_base. The window is decoded by the Super Buster
 * (superbuster_decode_z3 consults board_registry) and routed to the bus_ops
 * below via machine_rigel_bus.c's is_z3_board_addr -> expansion bus dispatch.
 * The descriptor is always linked in; enabled flips to 1 in the register call.
 * --------------------------------------------------------------------- */
static struct ExpansionBoard s_rtg_board = {
    .rom_file = s_rtg_config,
    .rom_size = BELLATRIX_RTG_WINDOW,
    .map_base = 0u,
    .is_z3    = 1u,
    .enabled  = 0u,
    .map      = NULL,
};
BELLATRIX_REGISTER_BOARD_Z3(s_rtg_board);

static uint32_t rtg_window_base(void) { return s_rtg_board.map_base; }

/* Per-access window serving (bus_ops). owns_address gates read/write, so both
 * assume addr lies in the window; bytes are assembled/split big-endian, which
 * feeds the byte-latch commit in rtg_write8. */
static int rtg_bus_owns(BellatrixExpansion *exp, uint32_t addr)
{
    uint32_t base = rtg_window_base();
    (void)exp;
    if (!base)
        return 0;
    return addr >= base && addr < base + BELLATRIX_RTG_WINDOW;
}

static uint32_t rtg_bus_read(BellatrixExpansion *exp, uint32_t addr,
                             unsigned int size)
{
    uint32_t off = addr - rtg_window_base();
    uint32_t v = 0u;
    unsigned i;
    (void)exp;
    for (i = 0; i < size; i++)
        v = (v << 8) | rtg_read8(NULL, off + i);
    return v;
}

static void rtg_bus_write(BellatrixExpansion *exp, uint32_t addr,
                          uint32_t value, unsigned int size)
{
    uint32_t off = addr - rtg_window_base();
    unsigned i;
    (void)exp;
    for (i = 0; i < size; i++)
        rtg_write8(NULL, off + i, (uint8_t)(value >> ((size - 1u - i) * 8u)));
}

static const BellatrixExpansionBusOps s_rtg_bus_ops = {
    .owns_address = rtg_bus_owns,
    .read         = rtg_bus_read,
    .write        = rtg_bus_write,
};

static void rtg_exp_reset(BellatrixExpansion *exp) { (void)exp; rtg_reset(NULL); }

static const BellatrixExpansionOps s_rtg_exp_ops = {
    .attach   = 0,
    .reset    = rtg_exp_reset,
    .shutdown = 0,
    .destroy  = 0,
};

int bellatrix_rtg_register(struct BellatrixMachine *m)
{
    uint8_t raw[AUTOCONFIG_ROM_BYTES];
    BellatrixExpansionDesc desc;

    memset(&s_rtg, 0, sizeof(s_rtg));
    memset(raw, 0, sizeof(raw));
    /* Zorro III board. The size in er_Type bits 2-0 is enough for our
     * fixed-size window; the Super Buster decode keys off the assigned base. */
    raw[0]  = (uint8_t)(AC_TYPE_Z3 | AC_TYPE_DIAGVALID | AC_SIZE_4MB);
    raw[1]  = BELLATRIX_RTG_PRODUCT;
    raw[4]  = (uint8_t)(BELLATRIX_RTG_MANUFACTURER >> 8);
    raw[5]  = (uint8_t)(BELLATRIX_RTG_MANUFACTURER & 0xFFu);
    raw[9]  = 0x02u;   /* serial */
    raw[10] = (uint8_t)(BELLATRIX_RTG_ROM_OFF >> 8);   /* InitDiagVec high */
    raw[11] = (uint8_t)(BELLATRIX_RTG_ROM_OFF & 0xFFu); /* InitDiagVec low  */
    autoconfig_build(s_rtg_config, raw);

    s_rtg_board.map_base = 0u;
    s_rtg_board.enabled  = 1u;

    /* Per-access serving via expansion.c bus_ops; no legacy zorro registry. */
    memset(&desc, 0, sizeof(desc));
    desc.id           = "bellatrix.rtg";
    desc.name         = "Bellatrix RTG";
    desc.kind         = BELLATRIX_EXPANSION_BOARD;
    desc.priority     = 80;
    desc.userdata     = &s_rtg;
    desc.zorro2_board = NULL;
    desc.bus_ops      = &s_rtg_bus_ops;
    desc.ops          = &s_rtg_exp_ops;

    if (bellatrix_expansion_register(m, &desc) != 0) {
        s_rtg_board.enabled = 0u;
        return -1;
    }
    s_registered = 1;
    kprintf("[RTG] board registered: Zorro III 4MB window, %u KB VRAM\n",
            (unsigned)(BELLATRIX_RTG_VRAM_SIZE / 1024u));
    return 0;
}

/* ----------------------------------------------------------------------- */
/* host-side frame access                                                   */
/* ----------------------------------------------------------------------- */

int bellatrix_rtg_active(void)
{
    return s_registered && s_rtg.enable &&
           s_rtg.mode_w >= 64u && s_rtg.mode_w <= 1920u &&
           s_rtg.mode_h >= 64u && s_rtg.mode_h <= 1080u &&
           s_rtg.bytes_per_row > 0u;
}

void bellatrix_rtg_frame_tick(void)
{
    s_rtg.vblank++;
}

uint8_t *bellatrix_rtg_vram_ptr(uint32_t *base, uint32_t *size)
{
    uint32_t board_base = rtg_window_base();
    if (!s_registered || !board_base)
        return 0;
    if (base) *base = board_base + BELLATRIX_RTG_VRAM_OFF;
    if (size) *size = BELLATRIX_RTG_VRAM_SIZE;
    return s_rtg.vram;
}

int bellatrix_rtg_get_frame(BellatrixRtgFrame *out)
{
    uint32_t w, h, bpr, x, y, start;
    const uint8_t *src;
    uint8_t *dst;

    if (!out || !bellatrix_rtg_active())
        return 0;

    w = s_rtg.mode_w;
    h = s_rtg.mode_h;
    bpr = s_rtg.bytes_per_row;
    start = s_rtg.pan;
    if (start >= BELLATRIX_RTG_VRAM_SIZE)
        start = 0;
    /* clamp height so we never read past VRAM */
    if ((uint64_t)start + (uint64_t)bpr * h > BELLATRIX_RTG_VRAM_SIZE)
        h = (uint32_t)((BELLATRIX_RTG_VRAM_SIZE - start) / (bpr ? bpr : 1u));
    if (h == 0)
        return 0;

    dst = s_rtg.frame;
    for (y = 0; y < h; y++) {
        src = s_rtg.vram + start + (size_t)y * bpr;
        switch (s_rtg.format) {
            case RTG_FMT_CLUT:
                for (x = 0; x < w; x++) {
                    uint32_t rgb = s_rtg.palette[src[x]];
                    *dst++ = (uint8_t)(rgb >> 16);
                    *dst++ = (uint8_t)(rgb >> 8);
                    *dst++ = (uint8_t)rgb;
                    *dst++ = 0xFF;
                }
                break;
            case RTG_FMT_R5G6B5:
                for (x = 0; x < w; x++) {
                    uint16_t p = (uint16_t)((src[x * 2] << 8) | src[x * 2 + 1]);
                    *dst++ = (uint8_t)(((p >> 11) & 0x1F) << 3);
                    *dst++ = (uint8_t)(((p >> 5)  & 0x3F) << 2);
                    *dst++ = (uint8_t)((p & 0x1F) << 3);
                    *dst++ = 0xFF;
                }
                break;
            case RTG_FMT_A8R8G8B8:
            default:
                for (x = 0; x < w; x++) {
                    *dst++ = src[x * 4 + 1];
                    *dst++ = src[x * 4 + 2];
                    *dst++ = src[x * 4 + 3];
                    *dst++ = 0xFF;
                }
                break;
        }
    }

    out->pixels = s_rtg.frame;
    out->width  = w;
    out->height = h;
    out->pitch  = w * 4u;
    return 1;
}
