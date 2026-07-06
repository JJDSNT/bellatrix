// src/machine/expansions/rtg/rtg.c — see rtg.h and docs/rtg_design.md.

#include "machine/expansions/rtg/rtg.h"
#include "machine/bus/zorro2/zorro2_bus.h"
#include "machine/autoconfig/autoconfig.h"
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

static const BellatrixZorro2BoardOps s_rtg_ops = {
    .reset  = rtg_reset,
    .destroy = 0,
    .read8  = rtg_read8,
    .write8 = rtg_write8,
};

static BellatrixZorro2BoardDesc s_rtg_desc;

int bellatrix_rtg_register(struct BellatrixMachine *m)
{
    uint8_t raw[AUTOCONFIG_ROM_BYTES];
    (void)m;

    memset(&s_rtg, 0, sizeof(s_rtg));
    memset(raw, 0, sizeof(raw));
    raw[0]  = (uint8_t)(AC_TYPE_Z2 | AC_TYPE_DIAGVALID | AC_SIZE_4MB);
    raw[1]  = BELLATRIX_RTG_PRODUCT;
    raw[4]  = (uint8_t)(BELLATRIX_RTG_MANUFACTURER >> 8);
    raw[5]  = (uint8_t)(BELLATRIX_RTG_MANUFACTURER & 0xFFu);
    raw[9]  = 0x02u;   /* serial */
    raw[10] = (uint8_t)(BELLATRIX_RTG_ROM_OFF >> 8);   /* InitDiagVec high */
    raw[11] = (uint8_t)(BELLATRIX_RTG_ROM_OFF & 0xFFu); /* InitDiagVec low  */
    autoconfig_build(s_rtg_config, raw);

    memset(&s_rtg_desc, 0, sizeof(s_rtg_desc));
    s_rtg_desc.id          = "bellatrix.rtg";
    s_rtg_desc.config_data = s_rtg_config;
    s_rtg_desc.config_size = sizeof(s_rtg_config);
    s_rtg_desc.window_size = BELLATRIX_RTG_WINDOW;
    s_rtg_desc.ops         = &s_rtg_ops;

    if (bellatrix_zorro2_register_board(&s_rtg_desc) != 0)
        return -1;
    s_registered = 1;
    kprintf("[RTG] board registered: 4MB window, %u KB VRAM\n",
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
    uint32_t board_base;
    if (!s_registered || !bellatrix_zorro2_board_configured("bellatrix.rtg"))
        return 0;
    board_base = bellatrix_zorro2_board_base("bellatrix.rtg");
    if (!board_base)
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
