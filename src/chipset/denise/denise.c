// src/chipset/denise/denise.c
//
// Denise — bitplane fetch and composite to VC4 framebuffer.
//
// ARM runs big-endian (-mbig-endian).  Chip RAM is also big-endian (M68K
// native), so uint16_t reads from chip RAM give the correct M68K word value
// directly.  The VC4 display controller expects little-endian pixels, so
// palette entries are pre-converted with LE16() at write time; the hot render
// path then writes them without any further byte manipulation.

#include "denise.h"
#include "chipset/agnus/agnus.h"
#include "support.h"
#include <stdint.h>
#include <string.h>

/* Chip RAM virtual base (Emu68 kernel space maps physical 0 here) */
#define CHIP_RAM_VIRT  0xffffff9000000000ULL
#define CHIP_RAM_MASK  0x001FFFFFUL

/* Framebuffer globals defined in emu68/src/aarch64/start.c */
extern uint16_t *framebuffer;
extern uint32_t  pitch;
extern uint32_t  fb_width;
extern uint32_t  fb_height;

/* ---------------------------------------------------------------------------
 * Colour conversion: Amiga 12-bit 0x0RGB → LE16 RGB565
 * ------------------------------------------------------------------------- */

static uint16_t amiga_color_to_le16(uint16_t amiga)
{
    uint8_t r4 = (amiga >> 8) & 0xF;
    uint8_t g4 = (amiga >> 4) & 0xF;
    uint8_t b4 = (amiga >> 0) & 0xF;
    uint8_t r8 = (r4 << 4) | r4;
    uint8_t g8 = (g4 << 4) | g4;
    uint8_t b8 = (b4 << 4) | b4;
    uint16_t rgb565 = (uint16_t)(((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3));
    return LE16(rgb565);
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

void denise_init(Denise *d)
{
    memset(d, 0, sizeof(*d));
}

void denise_reset(Denise *d)
{
    const AgnusState *saved_agnus = d->agnus;
    memset(d, 0, sizeof(*d));
    d->agnus = saved_agnus;
}

void denise_step(Denise *d, uint32_t ticks)
{
    (void)d;
    (void)ticks;
}

/* ---------------------------------------------------------------------------
 * Wiring
 * ------------------------------------------------------------------------- */

void denise_attach_agnus(Denise *d, const AgnusState *agnus)
{
    d->agnus = agnus;
}

/* ---------------------------------------------------------------------------
 * Bus protocol
 * ------------------------------------------------------------------------- */

int denise_handles_read(const Denise *d, uint32_t addr)
{
    (void)d;
    if (addr < 0xDFF000u || addr > 0xDFF1FFu)
        return 0;
    uint16_t reg = (uint16_t)(addr & 0x1FEu);
    return reg >= 0x0100u;
}

int denise_handles_write(const Denise *d, uint32_t addr)
{
    (void)d;
    if (addr < 0xDFF000u || addr > 0xDFF1FFu)
        return 0;
    uint16_t reg = (uint16_t)(addr & 0x1FEu);
    return reg >= 0x0100u;
}

uint32_t denise_read(Denise *d, uint32_t addr, unsigned int size)
{
    (void)d;
    (void)addr;
    (void)size;
    return 0;
}

void denise_write(Denise *d, uint32_t addr, uint32_t value, unsigned int size)
{
    (void)size;
    uint16_t reg = (uint16_t)(addr & 0x1FEu);
    denise_write_reg(d, reg, (uint16_t)value);
}

/* ---------------------------------------------------------------------------
 * Low-level register write
 * ------------------------------------------------------------------------- */

void denise_write_reg(Denise *d, uint16_t reg, uint16_t value)
{
    if (reg >= DENISE_COLOR_BASE && reg <= DENISE_COLOR_END && (reg & 1u) == 0u) {
        d->palette[(reg - DENISE_COLOR_BASE) >> 1] = amiga_color_to_le16(value);
        return;
    }

    switch (reg) {
    case DENISE_BPLCON0: d->bplcon0 = value;          return;
    case DENISE_BPLCON1: d->bplcon1 = value;          return;
    case DENISE_BPLCON2: d->bplcon2 = value;          return;
    case DENISE_BPL1MOD: d->bpl1mod = (int16_t)value; return;
    case DENISE_BPL2MOD: d->bpl2mod = (int16_t)value; return;
    default: return;
    }
}

/* ---------------------------------------------------------------------------
 * Frame render
 * ------------------------------------------------------------------------- */

static inline uint16_t chip_read16(uint32_t addr)
{
    return *(const volatile uint16_t *)(CHIP_RAM_VIRT + (addr & CHIP_RAM_MASK));
}

void denise_render_frame(Denise *d, const AgnusState *agnus)
{
    if (!framebuffer || !pitch) return;

    int nplanes = (d->bplcon0 >> 12) & 7;
    if (nplanes > 6) nplanes = 6;
    if (nplanes == 0) return;

    int hires = (d->bplcon0 >> 15) & 1;

    int vstrt   = (agnus->diwstrt >> 8) & 0xFF;
    int vstop   = (agnus->diwstop >> 8) & 0xFF;
    if (vstop <= vstrt) vstop += 256;
    int vheight = vstop - vstrt;
    if (vheight <= 0 || vheight > 512) return;

    int ddf_words = ((int)(agnus->ddfstop & 0xFE) - (int)(agnus->ddfstrt & 0xFC)) / 8 + 2;
    if (ddf_words < 1)  ddf_words = 20;
    if (ddf_words > 80) ddf_words = 80;
    int pix_per_line = ddf_words * 16;

    int scale = hires ? 1 : 2;
    int out_w = pix_per_line * scale;
    int out_h = vheight     * scale;

    uint32_t fb_x0 = ((uint32_t)out_w < fb_width)  ? (fb_width  - (uint32_t)out_w) / 2 : 0;
    uint32_t fb_y0 = ((uint32_t)out_h < fb_height) ? (fb_height - (uint32_t)out_h) / 2 : 0;

    uint32_t bpt[6];
    for (int p = 0; p < nplanes; p++) {
        bpt[p] = (((uint32_t)agnus->bplpth[p] & 0x001Fu) << 16)
               | ((uint32_t)agnus->bplptl[p] & 0xFFFEu);
        bpt[p] &= CHIP_RAM_MASK;
    }

    for (int line = 0; line < vheight; line++) {
        uint16_t plane_row[6][80];
        for (int p = 0; p < nplanes; p++)
            for (int w = 0; w < ddf_words; w++)
                plane_row[p][w] = chip_read16(bpt[p] + (uint32_t)(w * 2));

        for (int sy = 0; sy < scale; sy++) {
            uint32_t fb_y = fb_y0 + (uint32_t)(line * scale + sy);
            if (fb_y >= fb_height) break;
            uint16_t *row = (uint16_t *)((uintptr_t)framebuffer + fb_y * pitch);

            for (int w = 0; w < ddf_words; w++) {
                uint16_t pdata[6];
                for (int p = 0; p < nplanes; p++)
                    pdata[p] = plane_row[p][w];

                for (int b = 15; b >= 0; b--) {
                    int cidx = 0;
                    for (int p = 0; p < nplanes; p++)
                        cidx |= (((pdata[p] >> b) & 1) << p);

                    uint16_t pixel = d->palette[cidx & 31];
                    uint32_t fb_x = fb_x0 + (uint32_t)((w * 16 + (15 - b)) * scale);
                    for (int sx = 0; sx < scale; sx++)
                        if (fb_x + (uint32_t)sx < fb_width)
                            row[fb_x + (uint32_t)sx] = pixel;
                }
            }
        }

        for (int p = 0; p < nplanes; p++) {
            bpt[p] = (bpt[p] + (uint32_t)(ddf_words * 2)) & CHIP_RAM_MASK;
            int16_t mod = (p & 1) ? d->bpl2mod : d->bpl1mod;
            bpt[p] = (uint32_t)((int32_t)bpt[p] + (int32_t)mod) & CHIP_RAM_MASK;
        }
    }
}
