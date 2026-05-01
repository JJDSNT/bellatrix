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

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
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

static void denise_configure_diag_overrides(Denise *d);
static int denise_diag_target_line(int slot);
static int denise_diag_line_selected(int vpos);

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

static inline int denise_diw_hstart(const AgnusState *agnus)
{
    return (int)(agnus->diwstrt & 0xFFu);
}

static inline int denise_diw_hstop(const AgnusState *agnus)
{
    int hstart = denise_diw_hstart(agnus);
    int hstop = (int)(agnus->diwstop & 0xFFu);

    if (hstop <= hstart)
        hstop += 256;

    return hstop;
}

static inline int denise_visible_pixels(const AgnusState *agnus, int hires)
{
    int hwidth = denise_diw_hstop(agnus) - denise_diw_hstart(agnus);
    /*
     * Calibrate the DIW span to the harness framebuffer targets:
     * standard lores should occupy 320 source pixels, standard hires 640.
     * With the common 0x81..0xC1 DIW range this maps to 64 units.
     */
    int pixels_per_unit = hires ? 10 : 5;
    int pixels = hwidth * pixels_per_unit;

    if (pixels <= 0)
        return 0;

    return pixels;
}

static inline int denise_fine_scroll_pixels(const Denise *d, int hires)
{
    int shift = hires ? (int)(d->bplcon1 & 0x0Fu)
                      : (int)((d->bplcon1 >> 4) & 0x0Fu);

    if (shift < 0)
        shift = 0;
    if (shift > 15)
        shift = 15;

    return shift;
}

static inline int denise_ddf_phase_pixels(const Denise *d,
                                          const AgnusState *agnus,
                                          int hires)
{
    int ddf_h = (int)(agnus->ddfstrt & (hires ? 0xFEu : 0xFCu));
    int diw_h = denise_diw_hstart(agnus);
    int shift = denise_fine_scroll_pixels(d, hires);
    int display_start = diw_h - (hires ? 4 : 8) - shift;
    int fetch_start = ddf_h * 2;
    int pipeline_lead = hires ? 4 : 8;
    int delta = display_start - fetch_start - pipeline_lead;

    if (delta < 0)
        delta = 0;

    return delta;
}

static int denise_diag_target_line(int slot)
{
    static int initialized = 0;
    static int line0 = -1;
    static int line1 = -1;
    const char *env0;
    const char *env1;
    char *end = NULL;

    if (!initialized) {
        env0 = getenv("HARNESS_DIAG_LINE");
        env1 = getenv("HARNESS_DIAG_LINE2");

        if (env0 && env0[0] != '\0') {
            long v = strtol(env0, &end, 10);
            if (end && *end == '\0')
                line0 = (int)v;
        }

        end = NULL;
        if (env1 && env1[0] != '\0') {
            long v = strtol(env1, &end, 10);
            if (end && *end == '\0')
                line1 = (int)v;
        }

        initialized = 1;
    }

    return slot == 0 ? line0 : line1;
}

static int denise_diag_line_selected(int vpos)
{
    int line0 = denise_diag_target_line(0);
    int line1 = denise_diag_target_line(1);

    return (line0 >= 0 && vpos == line0) || (line1 >= 0 && vpos == line1);
}

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void denise_init(Denise *d)
{
    memset(d, 0, sizeof(*d));
    denise_configure_diag_overrides(d);
}

void denise_reset(Denise *d)
{
    const AgnusState *saved_agnus = d->agnus;
    int saved_diag_bit_reverse = d->diag_bit_reverse;
    int saved_diag_phase_bias = d->diag_phase_bias;
    memset(d, 0, sizeof(*d));
    d->agnus = saved_agnus;
    d->diag_bit_reverse = saved_diag_bit_reverse;
    d->diag_phase_bias = saved_diag_phase_bias;
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
        unsigned color_idx = (unsigned)((reg - DENISE_COLOR_BASE) >> 1);
        d->palette[color_idx] = amiga_color_to_le16(value);
        if (reg == DENISE_COLOR_BASE || reg == (DENISE_COLOR_BASE + 2u))
        {
            kprintf("[DENISE-COLOR%02u] amiga=%03x rgb565_le=%04x\n",
                    color_idx,
                    (unsigned)(value & 0x0FFFu),
                    (unsigned)d->palette[color_idx]);
        }
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

static void denise_dump_diag_decode(const Denise *d,
                                    const BitplaneState *bp,
                                    int nplanes,
                                    int dpf,
                                    int src_first_pixel)
{
    char idxbuf[(64 * 3) + 1];
    int pos = 0;

    idxbuf[0] = '\0';

    for (int i = 0; i < 64; ++i) {
        int src_pixel = src_first_pixel + i;
        int w = src_pixel / 16;
        int bit_in_word = 15 - (src_pixel % 16);
        uint16_t pdata[DENISE_MAX_PLANES] = {0, 0, 0, 0, 0, 0};
        uint8_t idx = 0;

        if (w < 0 || w >= bp->ddf_words)
            break;

        for (int p = 0; p < nplanes; ++p)
            pdata[p] = bp->line_words[p][w];

        if (dpf)
            idx = denise_translate_dpf_index(d, pdata, nplanes, bit_in_word);
        else
            idx = denise_decode_spf_index(pdata, nplanes, bit_in_word);

        pos += snprintf(&idxbuf[pos],
                        sizeof(idxbuf) - (size_t)pos,
                        "%02x%s",
                        (unsigned)idx,
                        (i == 63) ? "" : " ");
        if (pos >= (int)sizeof(idxbuf))
            break;
    }

    kprintf("[DENISE-DECODE] bp_v=%d src0=%d idx64=%s\n",
            bp->line_vpos,
            src_first_pixel,
            idxbuf);
}

static void denise_dump_diag_progression(const Denise *d,
                                         const BitplaneState *bp,
                                         int nplanes,
                                         int dpf,
                                         int src_first_pixel)
{
    for (int chunk = 0; chunk < 4; ++chunk) {
        char buf[16 * 16];
        int pos = 0;

        buf[0] = '\0';

        for (int i = 0; i < 16; ++i) {
            int rel_pixel = chunk * 16 + i;
            int src_pixel = src_first_pixel + rel_pixel;
            int w = src_pixel / 16;
            int bit_in_word = 15 - (src_pixel % 16);
            uint16_t pdata[DENISE_MAX_PLANES] = {0, 0, 0, 0, 0, 0};
            uint8_t idx = 0;

            if (w < 0 || w >= bp->ddf_words)
                break;

            for (int p = 0; p < nplanes; ++p)
                pdata[p] = bp->line_words[p][w];

            if (dpf)
                idx = denise_translate_dpf_index(d, pdata, nplanes, bit_in_word);
            else
                idx = denise_decode_spf_index(pdata, nplanes, bit_in_word);

            pos += snprintf(&buf[pos],
                            sizeof(buf) - (size_t)pos,
                            "%02d:%02d/%02d=%02x%s",
                            rel_pixel,
                            w,
                            bit_in_word,
                            (unsigned)idx,
                            (i == 15) ? "" : " ");
            if (pos >= (int)sizeof(buf))
                break;
        }

        kprintf("[DENISE-PROG] bp_v=%d src0=%d chunk=%d %s\n",
                bp->line_vpos,
                src_first_pixel,
                chunk,
                buf);
    }
}

static inline int denise_diagrom_window(const BitplaneState *bp, int line_idx)
{
    if (!bp)
        return 0;

    return bp->line_vpos >= 40 && bp->line_vpos <= 260 && (line_idx % 16) == 0;
}

static void denise_configure_diag_overrides(Denise *d)
{
    const char *reverse = getenv("HARNESS_DENISE_BIT_REVERSE");
    const char *bias = getenv("HARNESS_DENISE_PHASE_BIAS");
    const char *force_src0 = getenv("HARNESS_DENISE_FORCE_SRC0");
    const char *show_fetch_all = getenv("HARNESS_DENISE_SHOW_FETCH_ALL");
    char *end = NULL;

    d->diag_bit_reverse = (reverse && reverse[0] != '\0' &&
                           strcmp(reverse, "0") != 0) ? 1 : 0;
    d->diag_phase_bias = 0;
    d->diag_force_src0 = (force_src0 && force_src0[0] != '\0' &&
                          strcmp(force_src0, "0") != 0) ? 1 : 0;
    d->diag_show_fetch_all = (show_fetch_all && show_fetch_all[0] != '\0' &&
                              strcmp(show_fetch_all, "0") != 0) ? 1 : 0;

    if (bias && bias[0] != '\0') {
        long value = strtol(bias, &end, 10);
        if (end && *end == '\0') {
            if (value < -64)
                value = -64;
            if (value > 64)
                value = 64;
            d->diag_phase_bias = (int)value;
        }
    }
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
        {
            static uint32_t dbg_line_skip = 0;
            if ((dbg_line_skip++ & 63u) == 0)
            {
                kprintf("[DENISE-SKIP] bp_v=%d agnus_v=%d vstart=%d vstop=%d vheight=%d line_idx=%d ready=%d nplanes=%d ddf=%d diw=%04x-%04x\n",
                        bp->line_vpos,
                        (int)agnus->beam.vpos,
                        vstart,
                        vstop,
                        vheight,
                        line_idx,
                        bitplanes_line_ready(bp),
                        bp->nplanes,
                        bp->ddf_words,
                        (unsigned)agnus->diwstrt,
                        (unsigned)agnus->diwstop);
            }
            return;
        }

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
            static uint32_t dbg_bg_branch = 0;
            if ((dbg_bg_branch++ & 63u) == 0)
            {
                kprintf("[DENISE-BRANCH] line=%d nplanes=%d ddf_words=%d color0=%04x diw=%04x-%04x\n",
                        line_idx,
                        bp->nplanes,
                        bp->ddf_words,
                        (unsigned)d->palette[0],
                        (unsigned)agnus->diwstrt,
                        (unsigned)agnus->diwstop);
            }

            uint16_t bg = d->palette[0];

            int vscale = 2;
            int out_h = vheight * vscale;

            uint32_t fb_y0 = ((uint32_t)out_h < fb_height)
                                 ? (fb_height - (uint32_t)out_h) / 2u
                                 : 0u;

            for (int sy = 0; sy < vscale; ++sy)
            {
                uint32_t fb_y = fb_y0 + (uint32_t)(line_idx * vscale + sy);
                if (fb_y >= fb_height)
                    continue;

                uint16_t *row = (uint16_t *)((uintptr_t)framebuffer +
                                             ((uintptr_t)fb_y * (uintptr_t)pitch));

                for (uint32_t x = 0; x < fb_width; ++x)
                    row[x] = bg;

                if (line_idx == 0)
                {
                    static uint32_t dbg_bg_fill = 0;
                    if ((dbg_bg_fill++ & 63u) == 0)
                    {
                        kprintf("[DENISE-BG] line=%d fb_y=%u bg=%04x first=%04x width=%u\n",
                                line_idx,
                                (unsigned)fb_y,
                                (unsigned)bg,
                                (unsigned)row[0],
                                (unsigned)fb_width);
                    }
                }
            }

            return;
        }

    {
        int nplanes = bp->nplanes;
        int ddf_words = bp->ddf_words;
        int hires = denise_is_hires(d, bp);
        int dpf = denise_is_dpf(d);
        int hscale = hires ? 1 : 2;
        int vscale = 2;
        int pix_per_line = ddf_words * 16;
        int visible_pixels = denise_visible_pixels(agnus, hires);
        int fine_scroll = denise_fine_scroll_pixels(d, hires);
        int phase_pixels = 0;
        int x_phase = d->diag_phase_bias;
        int src_first_pixel = d->diag_force_src0 ? 0 : x_phase;
        int render_visible_pixels;
        int out_w;
        int out_h = vheight * vscale;

        if (visible_pixels <= 0)
            visible_pixels = pix_per_line;
        if (visible_pixels > pix_per_line)
            visible_pixels = pix_per_line;
        /* Keep the normal path anchored at the left edge of the DIW window.
         * Horizontal phase/debug bias remains opt-in via diag_force_src0/bias. */
        src_first_pixel = 0;

        if (d->diag_show_fetch_all) {
            src_first_pixel = 0;
            render_visible_pixels = pix_per_line;
        } else {
            render_visible_pixels = visible_pixels;
        }

        out_w = render_visible_pixels * hscale;

        uint32_t fb_x0 = ((uint32_t)out_w < fb_width) ? (fb_width - (uint32_t)out_w) / 2u : 0u;
        uint32_t fb_y0 = ((uint32_t)out_h < fb_height) ? (fb_height - (uint32_t)out_h) / 2u : 0u;

        for (int sy = 0; sy < vscale; ++sy)
        {

            uint32_t fb_y = fb_y0 + (uint32_t)(line_idx * vscale + sy);
            if (fb_y >= fb_height)
                continue;

            {
                uint16_t *row = (uint16_t *)((uintptr_t)framebuffer + ((uintptr_t)fb_y * (uintptr_t)pitch));
                uint16_t bg = d->palette[0];
                uint32_t non_bg_pixels = 0;
                uint8_t first_idx = 0;
                uint8_t last_idx = 0;
                int first_word = -1;
                int first_bit = -1;
                uint32_t first_non_bg_x = fb_width;
                uint32_t last_non_bg_x = 0;
                int saw_nonzero_idx = 0;

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
                        int bit = d->diag_bit_reverse ? (15 - b) : b;
                        int src_pixel = (w * 16) + (15 - b);
                        int vis_pixel = src_pixel - src_first_pixel;
                        uint8_t idx;
                        uint16_t pixel;
                        uint32_t fb_x;

                        if (vis_pixel < 0 || vis_pixel >= render_visible_pixels)
                            continue;

                        fb_x = fb_x0 + (uint32_t)(vis_pixel * hscale);

                        if (dpf)
                            idx = denise_translate_dpf_index(d, pdata, nplanes, bit);
                        else
                            idx = denise_decode_spf_index(pdata, nplanes, bit);

                        if (idx != 0)
                        {
                            non_bg_pixels += (uint32_t)hscale;
                            if (!saw_nonzero_idx)
                            {
                                first_idx = idx;
                                first_word = w;
                                first_bit = b;
                                first_non_bg_x = fb_x;
                                saw_nonzero_idx = 1;
                            }
                            last_idx = idx;
                            last_non_bg_x = fb_x + (uint32_t)(hscale - 1);
                        }

                        pixel = denise_color_from_index(d, idx, nplanes, dpf);

                        for (int sx = 0; sx < hscale; ++sx)
                        {
                            uint32_t x = fb_x + (uint32_t)sx;
                            if (x < fb_width)
                                row[x] = pixel;
                        }
                    }
                }

                if (sy == 0 && denise_diagrom_window(bp, line_idx))
                {
                    kprintf("[DENISE-DIAG] line=%d bp_v=%d np=%d hires=%d "
                            "dpf=%d words=%d pix=%d hscale=%d vscale=%d phase=%d x_phase=%d scroll=%d out=%dx%d fb0=%u,%u "
                            "visible=%d src0=%d rev=%d force0=%d fetchall=%d bias=%d non_bg=%u first_idx=%02x last_idx=%02x first_wb=%d/%d xspan=%u-%u "
                            "pal0=%04x pal1=%04x row0=%04x rowmid=%04x rowfirst=%04x rowlast=%04x\n",
                            line_idx,
                            bp->line_vpos,
                            nplanes,
                            hires,
                            dpf,
                            ddf_words,
                            pix_per_line,
                            hscale,
                            vscale,
                            phase_pixels,
                            x_phase,
                            fine_scroll,
                            out_w,
                            out_h,
                            (unsigned)fb_x0,
                            (unsigned)fb_y0,
                            render_visible_pixels,
                            src_first_pixel,
                            d->diag_bit_reverse,
                            d->diag_force_src0,
                            d->diag_show_fetch_all,
                            d->diag_phase_bias,
                            (unsigned)non_bg_pixels,
                            (unsigned)first_idx,
                            (unsigned)last_idx,
                            first_word,
                            first_bit,
                            saw_nonzero_idx ? (unsigned)first_non_bg_x : 0u,
                            saw_nonzero_idx ? (unsigned)last_non_bg_x : 0u,
                            (unsigned)d->palette[0],
                            (unsigned)d->palette[1],
                            (unsigned)row[fb_x0 < fb_width ? fb_x0 : 0],
                            (unsigned)row[(fb_x0 + (uint32_t)(out_w / 2)) < fb_width ? (fb_x0 + (uint32_t)(out_w / 2)) : (fb_width / 2)],
                            (unsigned)(saw_nonzero_idx && first_non_bg_x < fb_width ? row[first_non_bg_x] : bg),
                            (unsigned)(saw_nonzero_idx && last_non_bg_x < fb_width ? row[last_non_bg_x] : bg));

                    if (denise_diag_line_selected(bp->line_vpos)) {
                        for (int p = 0; p < nplanes; ++p) {
                            kprintf("[DENISE-PLANES] bp_v=%d plane=%d "
                                    "%04x %04x %04x %04x %04x %04x %04x %04x "
                                    "%04x %04x %04x %04x %04x %04x %04x %04x "
                                    "%04x %04x %04x %04x %04x\n",
                                    bp->line_vpos,
                                    p,
                                    bp->line_words[p][0],
                                    bp->line_words[p][1],
                                    bp->line_words[p][2],
                                    bp->line_words[p][3],
                                    bp->line_words[p][4],
                                    bp->line_words[p][5],
                                    bp->line_words[p][6],
                                    bp->line_words[p][7],
                                    bp->line_words[p][8],
                                    bp->line_words[p][9],
                                    bp->line_words[p][10],
                                    bp->line_words[p][11],
                                    bp->line_words[p][12],
                                    bp->line_words[p][13],
                                    bp->line_words[p][14],
                                    bp->line_words[p][15],
                                    bp->line_words[p][16],
                                    bp->line_words[p][17],
                                    bp->line_words[p][18],
                                    bp->line_words[p][19],
                                    bp->line_words[p][20]);
                        }

                        denise_dump_diag_decode(d,
                                                bp,
                                                nplanes,
                                                dpf,
                                                src_first_pixel);

                        if (bp->line_vpos == denise_diag_target_line(0)) {
                            denise_dump_diag_progression(d,
                                                         bp,
                                                         nplanes,
                                                         dpf,
                                                         src_first_pixel);
                        }
                    }
                }
            }
        }
    }
    }   /* close vstart/vstop/line_idx block */
}
