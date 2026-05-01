#include "bitplanes.h"

#include <stdlib.h>
#include <string.h>

#include "agnus.h"
#include "memory/memory.h"
#include "support.h"

#define CHIP_RAM_MASK BELLATRIX_CHIP_RAM_MASK

#ifndef DMAF_BPLEN
#define DMAF_BPLEN (1u << 8)
#endif

/* ---------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

static inline uint32_t make_bplpt(uint16_t hi, uint16_t lo)
{
    return (((uint32_t)(hi & 0x001Fu) << 16) |
            ((uint32_t)(lo & 0xFFFEu))) &
           CHIP_RAM_MASK;
}

static inline uint16_t bplpt_hi(uint32_t ptr)
{
    return (uint16_t)((ptr >> 16) & 0x001Fu);
}

static inline uint16_t bplpt_lo(uint32_t ptr)
{
    return (uint16_t)(ptr & 0xFFFEu);
}

static inline int agnus_display_vstart(const AgnusState *agnus)
{
    return (agnus->diwstrt >> 8) & 0xFF;
}

static inline int agnus_display_vstop(const AgnusState *agnus)
{
    int vstart = agnus_display_vstart(agnus);
    int vstop = (agnus->diwstop >> 8) & 0xFF;

    if (vstop <= vstart)
        vstop += 256;

    return vstop;
}

static inline int agnus_bitplane_count(const AgnusState *agnus)
{
    int nplanes = (agnus->bplcon0 >> 12) & 0x7;

    if (nplanes < 0)
        nplanes = 0;
    if (nplanes > 6)
        nplanes = 6;

    return nplanes;
}

static inline int agnus_hires_mode(const AgnusState *agnus)
{
    return (agnus->bplcon0 & 0x8000u) ? 1 : 0;
}

static inline int agnus_ddf_words(const AgnusState *agnus)
{
    int hires = agnus_hires_mode(agnus);
    int fetch_quantum = hires ? 4 : 8;
    int start = (int)(agnus->ddfstrt & (hires ? 0xFEu : 0xFCu));
    int stop = (int)(agnus->ddfstop & 0xFEu);
    int words;

    /*
     * Bellatrix currently renders each fetched word as 16 source pixels.
     * For the DiagROM lowres 320px setup (DDFSTRT=$38, DDFSTOP=$d0),
     * Agnus must expose exactly 20 words per line. The previous generic
     * +2 formula over-fetched one extra lowres word, advancing BPL pointers
     * by 2 bytes per line and producing the visible diagonal skew.
     *
     * Keep the existing hires behaviour for now and tighten only lowres.
     */
    words = ((stop - start) / fetch_quantum) + (hires ? 2 : 1);

    if (words < 1)
        words = 20;
    if (words > 80)
        words = 80;

    return words;
}

static inline int agnus_bitplane_dma_enabled(const AgnusState *agnus)
{
    if ((agnus->dmacon & DMAF_DMAEN) == 0)
        return 0;
    if ((agnus->dmacon & DMAF_BPLEN) == 0)
    {
        /*
         * Compatibility fallback:
         *
         * Some current Bellatrix boot paths reach a valid Copper display state
         * (BPLCON0/BPLxPT/DIW/DDF) without ever observing the expected DMACON
         * BPLEN write. Real hardware requires BPLEN, but allowing the fetch
         * when display state is otherwise coherent lets us validate the rest of
         * the display pipeline and unblocks the Happy Hand bring-up.
         *
         * Keep this narrowly scoped to cases that look like genuine bitplane
         * display setup instead of enabling background fetch unconditionally.
         */
        int nplanes = agnus_bitplane_count(agnus);
        if (nplanes <= 0)
            return 0;

        for (int p = 0; p < nplanes; ++p)
        {
            uint32_t ptr = make_bplpt(agnus->bplpth[p], agnus->bplptl[p]);
            if (ptr != 0)
            {
                static uint32_t dbg_forced_bpl_dma = 0;
                if ((dbg_forced_bpl_dma++ & 63u) == 0)
                {
                    kprintf("[BPL-DMA-FORCE] v=%u h=%u dmacon=%04x bplcon0=%04x plane=%d ptr=%05x\n",
                            (unsigned)agnus->beam.vpos,
                            (unsigned)agnus->beam.hpos,
                            (unsigned)agnus->dmacon,
                            (unsigned)agnus->bplcon0,
                            p,
                            (unsigned)ptr);
                }
                return 1;
            }
        }
        return 0;
    }
    return 1;
}

static inline int bitplanes_word_fetch_hpos(const BitplaneState *bp, int word_index)
{
    /*
     * Pragmatic fetch model:
     * one word every fetch quantum starting at DDFSTRT.
     *
     * Lowres: 8 beam positions per word
     * Hires:  4 beam positions per word
     *
     * This is still approximate, but much better than collapsing the whole
     * line fetch into a single moment.
     */
    int fetch_quantum = bp->hires ? 4 : 8;
    return bp->fetch_hstart + (word_index * fetch_quantum);
}

static inline int bitplanes_diagrom_window(const BitplaneState *bp,
                                           const AgnusState *agnus)
{
    if (!bp || !agnus)
        return 0;

    if (agnus->bplcon0 == 0)
        return 0;

    return bp->line_vpos >= 40 && bp->line_vpos <= 260;
}

static int bitplanes_diag_target_line(int slot)
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

static int bitplanes_diag_line_selected(int vpos)
{
    int line0 = bitplanes_diag_target_line(0);
    int line1 = bitplanes_diag_target_line(1);

    return (line0 >= 0 && vpos == line0) || (line1 >= 0 && vpos == line1);
}

static void bitplanes_snapshot_line_ptrs(BitplaneState *bp, const AgnusState *agnus)
{
    for (int p = 0; p < 6; ++p)
        bp->cur_bplpt[p] = make_bplpt(agnus->bplpth[p], agnus->bplptl[p]);
}

static void bitplanes_debug_dump_ptrs(const BitplaneState *bp, const AgnusState *agnus)
{
    static uint32_t dump_count = 0;

    if (!agnus || !agnus->memory)
        return;
    if (bp->nplanes < 2)
        return;
    if (agnus->bplcon0 != 0x2302u)
        return;
    if (bp->line_vpos < 44 || bp->line_vpos > 51)
        return;
    if (dump_count >= 8)
        return;

    for (int p = 0; p < bp->nplanes && p < 2; ++p)
    {
        uint32_t ptr = bp->cur_bplpt[p] & CHIP_RAM_MASK;
        uint16_t w0 = bellatrix_chip_read16(agnus->memory, ptr + 0u);
        uint16_t w1 = bellatrix_chip_read16(agnus->memory, ptr + 2u);
        uint16_t w2 = bellatrix_chip_read16(agnus->memory, ptr + 4u);
        uint16_t w3 = bellatrix_chip_read16(agnus->memory, ptr + 6u);

        kprintf("[BPL-PTR-DUMP] v=%d plane=%d ptr=%05x data=%04x %04x %04x %04x\n",
                bp->line_vpos,
                p,
                (unsigned)ptr,
                w0, w1, w2, w3);
    }

    dump_count++;
}

static void bitplanes_diagrom_dump_raw_planes(const BitplaneState *bp,
                                              const AgnusState *agnus)
{
    if (!bp || !agnus || !agnus->memory)
        return;
    if (bp->nplanes <= 0)
        return;
    if (!bitplanes_diag_line_selected(bp->line_vpos))
        return;

    for (int p = 0; p < bp->nplanes; ++p)
    {
        uint32_t ptr = bp->cur_bplpt[p] & CHIP_RAM_MASK;

        kprintf("[BPL-RAW] bp_v=%d plane=%d ptr=%05x "
                "%04x %04x %04x %04x %04x %04x %04x %04x "
                "%04x %04x %04x %04x %04x %04x %04x %04x "
                "%04x %04x %04x %04x %04x\n",
                bp->line_vpos,
                p,
                (unsigned)ptr,
                bellatrix_chip_read16(agnus->memory, ptr + 0u),
                bellatrix_chip_read16(agnus->memory, ptr + 2u),
                bellatrix_chip_read16(agnus->memory, ptr + 4u),
                bellatrix_chip_read16(agnus->memory, ptr + 6u),
                bellatrix_chip_read16(agnus->memory, ptr + 8u),
                bellatrix_chip_read16(agnus->memory, ptr + 10u),
                bellatrix_chip_read16(agnus->memory, ptr + 12u),
                bellatrix_chip_read16(agnus->memory, ptr + 14u),
                bellatrix_chip_read16(agnus->memory, ptr + 16u),
                bellatrix_chip_read16(agnus->memory, ptr + 18u),
                bellatrix_chip_read16(agnus->memory, ptr + 20u),
                bellatrix_chip_read16(agnus->memory, ptr + 22u),
                bellatrix_chip_read16(agnus->memory, ptr + 24u),
                bellatrix_chip_read16(agnus->memory, ptr + 26u),
                bellatrix_chip_read16(agnus->memory, ptr + 28u),
                bellatrix_chip_read16(agnus->memory, ptr + 30u),
                bellatrix_chip_read16(agnus->memory, ptr + 32u),
                bellatrix_chip_read16(agnus->memory, ptr + 34u),
                bellatrix_chip_read16(agnus->memory, ptr + 36u),
                bellatrix_chip_read16(agnus->memory, ptr + 38u),
                bellatrix_chip_read16(agnus->memory, ptr + 40u));
    }
}

static void bitplanes_publish_ptrs_to_agnus(const BitplaneState *bp, AgnusState *agnus)
{
    /*
     * Publish progressively advanced pointers back into Agnus shadow registers.
     * This preserves continuity line-to-line, while still allowing Copper to
     * overwrite pointers before the next line starts.
     */
    for (int p = 0; p < bp->nplanes; ++p)
    {
        agnus->bplpth[p] = bplpt_hi(bp->cur_bplpt[p]);
        agnus->bplptl[p] = bplpt_lo(bp->cur_bplpt[p]);
    }
}

static void bitplanes_apply_modulos(BitplaneState *bp, const AgnusState *agnus)
{
    /*
     * Odd planes (0, 2, 4) use BPL1MOD; even planes (1, 3, 5) use BPL2MOD.
     */
    for (int p = 0; p < bp->nplanes; ++p)
    {
        int16_t mod = (p & 1) ? agnus->bpl2mod : agnus->bpl1mod;
        bp->cur_bplpt[p] = (uint32_t)((int32_t)bp->cur_bplpt[p] + (int32_t)mod) & CHIP_RAM_MASK;
    }
}

void bitplanes_begin_line(BitplaneState *bp, const AgnusState *agnus, int vpos_abs)
{
    bp->active = 1;
    bp->line_ready = 0;
    bp->line_vpos = vpos_abs;
    bp->line_words_fetched = 0;
    bp->fetch_index = 0;

    /*
     * Re-evaluate dynamic display state per line so Copper updates can affect
     * subsequent lines.
     */
    bp->hires = agnus_hires_mode(agnus);
    bp->nplanes = agnus_bitplane_count(agnus);
    bp->ddf_words = agnus_ddf_words(agnus);

    if (!agnus_bitplane_dma_enabled(agnus))
        bp->nplanes = 0;
    bp->fetch_hstart = (int)(agnus->ddfstrt & 0xFF);
    bp->fetch_hstop = (int)(agnus->ddfstop & 0xFF);

    if (bitplanes_diagrom_window(bp, agnus) && ((vpos_abs - 40) % 16) == 0) {
        kprintf("[BPL-DIAG-BEGIN] v=%d h=%d bplcon0=%04x nplanes=%d "
                "bpl1=%05x bpl2=%05x bpl3=%05x diw=%04x/%04x ddf=%04x/%04x "
                "ddf_words=%d mod1=%04x mod2=%04x dmacon=%04x\n",
                vpos_abs,
                agnus->beam.hpos,
                agnus->bplcon0,
                bp->nplanes,
                make_bplpt(agnus->bplpth[0], agnus->bplptl[0]),
                make_bplpt(agnus->bplpth[1], agnus->bplptl[1]),
                make_bplpt(agnus->bplpth[2], agnus->bplptl[2]),
                agnus->diwstrt,
                agnus->diwstop,
                agnus->ddfstrt,
                agnus->ddfstop,
                bp->ddf_words,
                (uint16_t)agnus->bpl1mod,
                (uint16_t)agnus->bpl2mod,
                agnus->dmacon);
    }

    if (bp->nplanes <= 0)
    {
        /*
         * No bitplanes active: background-only line is immediately ready.
         * Leave active=1 so bitplanes_step does not re-enter begin_line for
         * the same vpos on subsequent agnus_step calls.
         */
        bp->line_ready = 1;
        return;
    }

    bitplanes_snapshot_line_ptrs(bp, agnus);
    bitplanes_debug_dump_ptrs(bp, agnus);
    bitplanes_diagrom_dump_raw_planes(bp, agnus);
}

static void bitplanes_fetch_word(BitplaneState *bp, AgnusState *agnus, int word_index)
{
    if (!agnus || !agnus->memory)
        return;
    if (bp->nplanes <= 0)
        return;
    if (word_index < 0 || word_index >= bp->ddf_words)
        return;

    for (int p = 0; p < bp->nplanes; ++p)
    {
        uint32_t addr = bp->cur_bplpt[p] & CHIP_RAM_MASK;
        bp->line_words[p][word_index] = bellatrix_chip_read16(agnus->memory, addr);
        bp->cur_bplpt[p] = (bp->cur_bplpt[p] + 2u) & CHIP_RAM_MASK;
    }

    if (bp->nplanes > 0 && bitplanes_diagrom_window(bp, agnus) &&
        (word_index == 0 || word_index == bp->ddf_words - 1))
    {
        uint16_t w0 = bp->line_words[0][word_index];
        uint16_t w1 = (bp->nplanes > 1) ? bp->line_words[1][word_index] : 0;
        uint16_t w2 = (bp->nplanes > 2) ? bp->line_words[2][word_index] : 0;

        kprintf("[BPL-DIAG-FETCH] v=%d h=%d target_h=%d wi=%d/%d np=%d "
                "w0=%04x w1=%04x w2=%04x next1=%05x next2=%05x next3=%05x\n",
                bp->line_vpos,
                agnus->beam.hpos,
                bitplanes_word_fetch_hpos(bp, word_index),
                word_index,
                bp->ddf_words,
                bp->nplanes,
                w0,
                w1,
                w2,
                bp->cur_bplpt[0],
                (bp->nplanes > 1) ? bp->cur_bplpt[1] : 0u,
                (bp->nplanes > 2) ? bp->cur_bplpt[2] : 0u);
    }

    bp->line_words_fetched = word_index + 1;
    bitplanes_publish_ptrs_to_agnus(bp, agnus);

    if (bp->line_words_fetched >= bp->ddf_words)
    {
        bp->line_ready = 1;
        bitplanes_apply_modulos(bp, agnus);
        bitplanes_publish_ptrs_to_agnus(bp, agnus);

        if (bp->nplanes > 0)
        {
            uint16_t first0 = bp->line_words[0][0];
            uint16_t last0 = bp->line_words[0][bp->ddf_words - 1];
            uint16_t first1 = (bp->nplanes > 1) ? bp->line_words[1][0] : 0;
            uint16_t last1 = (bp->nplanes > 1) ? bp->line_words[1][bp->ddf_words - 1] : 0;
            uint16_t first2 = (bp->nplanes > 2) ? bp->line_words[2][0] : 0;
            uint16_t last2 = (bp->nplanes > 2) ? bp->line_words[2][bp->ddf_words - 1] : 0;

            if (bitplanes_diagrom_window(bp, agnus) &&
                ((bp->line_vpos - 40) % 16) == 0) {
                kprintf("[BPL-DIAG-DONE] v=%d np=%d words=%d first0=%04x last0=%04x "
                        "first1=%04x last1=%04x first2=%04x last2=%04x mod1=%04x mod2=%04x "
                        "post1=%05x post2=%05x post3=%05x\n",
                    bp->line_vpos,
                    bp->nplanes,
                    bp->ddf_words,
                    first0,
                    last0,
                    first1,
                    last1,
                    first2,
                    last2,
                        (uint16_t)agnus->bpl1mod,
                        (uint16_t)agnus->bpl2mod,
                        bp->cur_bplpt[0],
                        (bp->nplanes > 1) ? bp->cur_bplpt[1] : 0u,
                        (bp->nplanes > 2) ? bp->cur_bplpt[2] : 0u);
            }
        }
    }
}

static void bitplanes_progress_fetch(BitplaneState *bp, AgnusState *agnus, int hpos)
{
    if (bp->fetch_index >= bp->ddf_words)
        return;

    while (bp->fetch_index < bp->ddf_words)
    {
        int target_hpos = bitplanes_word_fetch_hpos(bp, bp->fetch_index);

        if (hpos < target_hpos)
            break;

        bitplanes_fetch_word(bp, agnus, bp->fetch_index);
        bp->fetch_index++;
    }
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

void bitplanes_init(BitplaneState *bp)
{
    memset(bp, 0, sizeof(*bp));
    bp->line_vpos = -1;
}

void bitplanes_reset(BitplaneState *bp)
{
    bitplanes_init(bp);
}

/* ---------------------------------------------------------------------------
 * Frame / line flow
 * ------------------------------------------------------------------------- */

void bitplanes_begin_frame(BitplaneState *bp, const AgnusState *agnus,
                           int nplanes, int hires)
{
    (void)agnus;

    /*
     * Keep the existing API shape, but do not snapshot pointers here anymore.
     * All dynamic display state is now latched per line.
     */
    bp->active = 0;
    bp->line_ready = 0;
    bp->hires = hires;
    bp->nplanes = (nplanes > 6) ? 6 : ((nplanes < 0) ? 0 : nplanes);
    bp->ddf_words = 0;
    bp->line_words_fetched = 0;
    bp->fetch_index = 0;
    bp->line_vpos = -1;
    bp->fetch_hstart = 0;
    bp->fetch_hstop = 0;
}

void bitplanes_fetch_line(BitplaneState *bp, AgnusState *agnus, int vpos_abs)
{
    /*
     * Preserve API compatibility, but now implement it by fetching all still
     * missing words incrementally.
     */
    if (!agnus || !agnus->memory)
        return;
    if (bp->nplanes <= 0)
        return;

    bp->line_vpos = vpos_abs;

    while (bp->fetch_index < bp->ddf_words)
    {
        bitplanes_fetch_word(bp, agnus, bp->fetch_index);
        bp->fetch_index++;
    }
}

void bitplanes_step(BitplaneState *bp, AgnusState *agnus)
{
    int vstart, vstop, vpos, hpos;

    if (!agnus)
        return;

    vstart = agnus_display_vstart(agnus);
    vstop = agnus_display_vstop(agnus);
    vpos = (int)agnus->beam.vpos;
    hpos = (int)agnus->beam.hpos;

    if (vpos < vstart || vpos >= vstop)
    {
        bp->active = 0;
        return;
    }

    /*
     * New visible line: latch current Agnus bitplane configuration now,
     * not at frame start.
     */
    if (!bp->active || bp->line_vpos != vpos)
    {
        bitplanes_begin_line(bp, agnus, vpos);
    }

    if (bp->nplanes <= 0)
        return;

    /*
     * Incremental fetch model:
     * words become available as beam advances across DDF.
     */
    if (hpos >= bp->fetch_hstart && agnus->memory)
    {
        bitplanes_progress_fetch(bp, agnus, hpos);
    }

    /*
     * Keep the line latched until vpos changes. Clearing active here causes
     * the same scanline to restart repeatedly once fetch completes, advancing
     * BPL pointers many times inside a single raster line.
     */
}

void bitplanes_end_line(BitplaneState *bp, AgnusState *agnus)
{
    (void)agnus;
    bp->active = 0;
}

/* ---------------------------------------------------------------------------
 * Helpers for Denise
 * ------------------------------------------------------------------------- */

int bitplanes_line_ready(const BitplaneState *bp)
{
    return bp->line_ready;
}

void bitplanes_clear_line_ready(BitplaneState *bp)
{
    bp->line_ready = 0;
}

int bitplanes_nplanes(const BitplaneState *bp)
{
    return bp->nplanes;
}

int bitplanes_ddf_words(const BitplaneState *bp)
{
    return bp->ddf_words;
}

const uint16_t *bitplanes_plane_words(const BitplaneState *bp, int plane)
{
    if (plane < 0 || plane >= bp->nplanes)
        return 0;
    return bp->line_words[plane];
}

int bitplanes_line_vpos(const BitplaneState *bp)
{
    return bp->line_vpos;
}
