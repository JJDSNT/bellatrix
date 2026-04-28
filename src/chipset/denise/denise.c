// src/chipset/denise/denise.c
//
// Denise — bitplane compositor.
//
// Denise does NOT access chip RAM directly. Bitplane data is fetched by
// Agnus/bitplanes.c and handed to denise_render_line() one scanline at a time.
//
// This version keeps the Bellatrix bring-up shape, but improves the render
// pipeline:
//   - decode bitplanes -> palette index
//   - translate SPF / DPF
//   - colorize to framebuffer
//
// Sprites, HAM and collision logic are intentionally left for later.

#include "denise.h"
#include "chipset/agnus/agnus.h"
#include "support.h"

#include <stdint.h>
#include <string.h>

/* Framebuffer globals defined in emu68/src/aarch64/start.c */
extern uint16_t *framebuffer;
extern uint32_t pitch;
extern uint32_t fb_width;
extern uint32_t fb_height;

/* ------------------------------------------------------------------------- */
/* local constants                                                           */
/* ------------------------------------------------------------------------- */

#define DENISE_BPLCON0_DBLPF 0x0400u
#define DENISE_BPLCON2_PF2PRI 0x0040u

#define DENISE_MAX_PLANES 6
#define DENISE_MAX_WORDS 80
#define DENISE_MAX_PIXELS (DENISE_MAX_WORDS * 16)

/* ------------------------------------------------------------------------- */
/* Colour conversion helpers                                                 */
/* ------------------------------------------------------------------------- */

static uint16_t amiga_color_to_le16(uint16_t amiga)
{
    uint8_t r4 = (amiga >> 8) & 0xF;
    uint8_t g4 = (amiga >> 4) & 0xF;
    uint8_t b4 = (amiga >> 0) & 0xF;

    uint8_t r8 = (uint8_t)((r4 << 4) | r4);
    uint8_t g8 = (uint8_t)((g4 << 4) | g4);
    uint8_t b8 = (uint8_t)((b4 << 4) | b4);

    uint16_t rgb565 = (uint16_t)(((r8 >> 3) << 11) |
                                 ((g8 >> 2) << 5) |
                                 ((b8 >> 3) << 0));

    return LE16(rgb565);
}

static uint16_t denise_halfbrite_le16(uint16_t le_rgb565)
{
    uint16_t rgb565 = LE16(le_rgb565);

    uint16_t r = (rgb565 >> 11) & 0x1Fu;
    uint16_t g = (rgb565 >> 5) & 0x3Fu;
    uint16_t b = (rgb565 >> 0) & 0x1Fu;

    r >>= 1;
    g >>= 1;
    b >>= 1;

    return LE16((uint16_t)((r << 11) | (g << 5) | b));
}

/* ------------------------------------------------------------------------- */
/* small mode helpers                                                        */
/* ------------------------------------------------------------------------- */

static inline int denise_is_hires(const Denise *d, const BitplaneState *bp)
{
    (void)d;
    return bp->hires ? 1 : 0;
}

static inline int denise_is_dpf(const Denise *d)
{
    return (d->bplcon0 & DENISE_BPLCON0_DBLPF) ? 1 : 0;
}

static inline int denise_pf2_has_priority(const Denise *d)
{
    return (d->bplcon2 & DENISE_BPLCON2_PF2PRI) ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

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

/* ------------------------------------------------------------------------- */
/* wiring                                                                    */
/* ------------------------------------------------------------------------- */

void denise_attach_agnus(Denise *d, const AgnusState *agnus)
{
    d->agnus = agnus;
}

/* ------------------------------------------------------------------------- */
/* bus protocol                                                              */
/* ------------------------------------------------------------------------- */

int denise_handles_read(const Denise *d, uint32_t addr)
{
    (void)d;

    if (addr < 0xDFF000u || addr > 0xDFF1FFu)
        return 0;

    {
        uint16_t reg = (uint16_t)(addr & 0x1FEu);
        return reg >= 0x0100u;
    }
}

int denise_handles_write(const Denise *d, uint32_t addr)
{
    (void)d;

    if (addr < 0xDFF000u || addr > 0xDFF1FFu)
        return 0;

    {
        uint16_t reg = (uint16_t)(addr & 0x1FEu);

        /* BPL1MOD/BPL2MOD are Agnus registers, not Denise */
        if (reg == 0x0108u || reg == 0x010Au)
            return 0;

        return reg >= 0x0100u;
    }
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
    denise_write_reg(d, (uint16_t)(addr & 0x1FEu), (uint16_t)value);
}

/* ------------------------------------------------------------------------- */
/* low-level register write                                                  */
/* ------------------------------------------------------------------------- */

void denise_write_reg(Denise *d, uint16_t reg, uint16_t value)
{
    if (reg >= DENISE_COLOR_BASE &&
        reg <= DENISE_COLOR_END &&
        (reg & 1u) == 0u)
    {
        d->palette[(reg - DENISE_COLOR_BASE) >> 1] = amiga_color_to_le16(value);
        return;
    }

    switch (reg)
    {
    case DENISE_BPLCON0:
        d->bplcon0 = value;
        kprintf("[DENISE] BPLCON0=0x%04x nplanes=%d hires=%d dblpf=%d\n",
                (unsigned)value,
                (int)((value >> 12) & 7),
                (int)((value >> 15) & 1),
                (int)((value & DENISE_BPLCON0_DBLPF) ? 1 : 0));
        return;

    case DENISE_BPLCON1:
        d->bplcon1 = value;
        return;

    case DENISE_BPLCON2:
        d->bplcon2 = value;
        return;

    default:
        return;
    }
}

/* ------------------------------------------------------------------------- */
/* decode helpers                                                            */
/* ------------------------------------------------------------------------- */

static inline uint8_t denise_decode_spf_index(const uint16_t pdata[DENISE_MAX_PLANES],
                                              int nplanes, int bit)
{
    uint8_t idx = 0;

    for (int p = 0; p < nplanes; ++p)
        idx |= (uint8_t)(((pdata[p] >> bit) & 1u) << p);

    return idx;
}

static inline uint8_t denise_decode_dpf_pf1(const uint16_t pdata[DENISE_MAX_PLANES],
                                            int nplanes, int bit)
{
    uint8_t idx = 0;

    if (nplanes > 0)
        idx |= (uint8_t)(((pdata[0] >> bit) & 1u) << 0);
    if (nplanes > 2)
        idx |= (uint8_t)(((pdata[2] >> bit) & 1u) << 1);
    if (nplanes > 4)
        idx |= (uint8_t)(((pdata[4] >> bit) & 1u) << 2);

    return idx;
}

static inline uint8_t denise_decode_dpf_pf2(const uint16_t pdata[DENISE_MAX_PLANES],
                                            int nplanes, int bit)
{
    uint8_t idx = 0;

    if (nplanes > 1)
        idx |= (uint8_t)(((pdata[1] >> bit) & 1u) << 0);
    if (nplanes > 3)
        idx |= (uint8_t)(((pdata[3] >> bit) & 1u) << 1);
    if (nplanes > 5)
        idx |= (uint8_t)(((pdata[5] >> bit) & 1u) << 2);

    return idx;
}

static inline uint8_t denise_translate_dpf_index(const Denise *d,
                                                 const uint16_t pdata[DENISE_MAX_PLANES],
                                                 int nplanes, int bit)
{
    uint8_t pf1 = denise_decode_dpf_pf1(pdata, nplanes, bit);
    uint8_t pf2 = denise_decode_dpf_pf2(pdata, nplanes, bit);

    if (pf1)
    {
        if (pf2)
        {
            return denise_pf2_has_priority(d) ? (uint8_t)(pf2 | 0x08u) : pf1;
        }
        return pf1;
    }

    if (pf2)
        return (uint8_t)(pf2 | 0x08u);

    return 0;
}

static inline uint16_t denise_color_from_index(const Denise *d,
                                               uint8_t idx,
                                               int nplanes,
                                               int dpf)
{
    /*
     * Single playfield:
     *   - up to 5 planes: direct palette lookup
     *   - 6 planes: treat bit 5 as EHB half-bright
     *
     * Dual playfield:
     *   - translated indices already map into 0..15
     */
    if (!dpf && nplanes == 6)
    {
        if (idx & 0x20u)
            return denise_halfbrite_le16(d->palette[idx & 0x1Fu]);
        return d->palette[idx & 0x1Fu];
    }

    return d->palette[idx & 0x1Fu];
}

/* ------------------------------------------------------------------------- */
/* render                                                                    */
/* ------------------------------------------------------------------------- */

void denise_render_line(Denise *d, const AgnusState *agnus,
                        const BitplaneState *bp)
{
    if (!framebuffer || !pitch)
        return;

    if (!agnus || !bp)
        return;

    {
        int vstart = (agnus->diwstrt >> 8) & 0xFF;
        int vstop  = (agnus->diwstop >> 8) & 0xFF;
        if (vstop <= vstart)
            vstop += 256;
        int vheight = vstop - vstart;
        if (vheight <= 0)
            vheight = 1;
        if (vheight > 512)
            vheight = 512;

        int line_idx = bp->line_vpos - vstart;
        if (line_idx < 0 || line_idx >= vheight)
            return;

        static uint32_t dbg_render_calls = 0;
        if ((dbg_render_calls++ & 63u) == 0)
        {
            kprintf("[DENISE-ENTRY] call=%u ready=%d bp_nplanes=%d bp_line_vpos=%d agnus_v=%d bplcon0=%04x color0=%04x\n",
                    (unsigned)dbg_render_calls,
                    bitplanes_line_ready(bp),
                    bitplanes_nplanes(bp),
                    bitplanes_line_vpos(bp),
                    (int)agnus->beam.vpos,
                    (unsigned)d->bplcon0,
                    (unsigned)d->palette[0]);
        }

        /*
         * Even with zero bitplanes, Denise must output COLOR00.
         * This is exactly the AROS/amigavideo initial copperlist case:
         * BPLCON0=0x0200, COLOR00=0x0aaa, no active bitplanes yet.
         */
        if (bp->nplanes <= 0 || bp->ddf_words <= 0)
        {
            uint16_t bg = d->palette[0];

            int scale = 2;
            int out_h = vheight * scale;

            uint32_t fb_y0 = ((uint32_t)out_h < fb_height)
                                 ? (fb_height - (uint32_t)out_h) / 2u
                                 : 0u;

            for (int sy = 0; sy < scale; ++sy)
            {
                uint32_t fb_y = fb_y0 + (uint32_t)(line_idx * scale + sy);
                if (fb_y >= fb_height)
                    continue;

                uint16_t *row = (uint16_t *)((uintptr_t)framebuffer +
                                             ((uintptr_t)fb_y * (uintptr_t)pitch));

                for (uint32_t x = 0; x < fb_width; ++x)
                    row[x] = bg;
            }

            return;
        }

    {
        int nplanes = bp->nplanes;
        int ddf_words = bp->ddf_words;
        int hires = denise_is_hires(d, bp);
        int dpf = denise_is_dpf(d);
        int scale = hires ? 1 : 2;

        int pix_per_line = ddf_words * 16;
        int out_w = pix_per_line * scale;
        int out_h = vheight * scale;

        uint32_t fb_x0 = ((uint32_t)out_w < fb_width) ? (fb_width - (uint32_t)out_w) / 2u : 0u;
        uint32_t fb_y0 = ((uint32_t)out_h < fb_height) ? (fb_height - (uint32_t)out_h) / 2u : 0u;

        for (int sy = 0; sy < scale; ++sy)
        {

            uint32_t fb_y = fb_y0 + (uint32_t)(line_idx * scale + sy);
            if (fb_y >= fb_height)
                continue;

            {
                uint16_t *row = (uint16_t *)((uintptr_t)framebuffer + ((uintptr_t)fb_y * (uintptr_t)pitch));
                uint16_t bg = d->palette[0];

                /*
                 * Clear the whole line to background first so narrower display
                 * windows don't leave stale pixels behind.
                 */
                for (uint32_t x = 0; x < fb_width; ++x)
                    row[x] = bg;

                for (int w = 0; w < ddf_words; ++w)
                {

                    uint16_t pdata[DENISE_MAX_PLANES] = {0, 0, 0, 0, 0, 0};

                    for (int p = 0; p < nplanes; ++p)
                        pdata[p] = bp->line_words[p][w];

                    for (int b = 15; b >= 0; --b)
                    {

                        uint8_t idx;
                        uint16_t pixel;
                        uint32_t fb_x;

                        if (dpf)
                            idx = denise_translate_dpf_index(d, pdata, nplanes, b);
                        else
                            idx = denise_decode_spf_index(pdata, nplanes, b);

                        pixel = denise_color_from_index(d, idx, nplanes, dpf);
                        fb_x = fb_x0 + (uint32_t)((w * 16 + (15 - b)) * scale);

                        for (int sx = 0; sx < scale; ++sx)
                        {
                            uint32_t x = fb_x + (uint32_t)sx;
                            if (x < fb_width)
                                row[x] = pixel;
                        }
                    }
                }
            }
        }
    }
    }   /* close vstart/vstop/line_idx block */
}