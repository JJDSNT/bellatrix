#include "bitplanes.h"

#include <string.h>

#include "agnus.h"
#include "memory/memory.h"
#include "support.h"

#define CHIP_RAM_MASK 0x001FFFFFu

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
    int words = ((int)(agnus->ddfstop & 0xFE) - (int)(agnus->ddfstrt & 0xFC)) / 8 + 2;

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
        return 0;
    return 1;
}

static inline int bitplanes_word_fetch_hpos(const BitplaneState *bp, int word_index)
{
    /*
     * Pragmatic fetch model:
     * one word every 8 lowres beam positions starting at DDFSTRT.
     *
     * This is still approximate, but much better than collapsing the whole
     * line fetch into a single moment.
     */
    return bp->fetch_hstart + (word_index * 8);
}

static void bitplanes_snapshot_line_ptrs(BitplaneState *bp, const AgnusState *agnus)
{
    for (int p = 0; p < 6; ++p)
        bp->cur_bplpt[p] = make_bplpt(agnus->bplpth[p], agnus->bplptl[p]);
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
    bp->fetch_hstart = (int)(agnus->ddfstrt & 0xFF);
    bp->fetch_hstop = (int)(agnus->ddfstop & 0xFF);

    kprintf("[BPL-LINE-BEGIN] v=%d h=%d bplcon0=%04x nplanes=%d "
            "bpl1=%05x bpl2=%05x diw=%04x/%04x ddf=%04x/%04x dmacon=%04x\n",
            vpos_abs,
            agnus->beam.hpos,
            agnus->bplcon0,
            bp->nplanes,
            make_bplpt(agnus->bplpth[0], agnus->bplptl[0]),
            make_bplpt(agnus->bplpth[1], agnus->bplptl[1]),
            agnus->diwstrt,
            agnus->diwstop,
            agnus->ddfstrt,
            agnus->ddfstop,
            agnus->dmacon);

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

    bp->line_words_fetched = word_index + 1;
    bitplanes_publish_ptrs_to_agnus(bp, agnus);

    if (bp->line_words_fetched >= bp->ddf_words)
    {
        bp->line_ready = 1;
        bitplanes_apply_modulos(bp, agnus);
        bitplanes_publish_ptrs_to_agnus(bp, agnus);
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

    if (!agnus_bitplane_dma_enabled(agnus))
    {
        bp->active = 0;
        return;
    }

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

    if (bp->active &&
        bp->fetch_index >= bp->ddf_words &&
        hpos >= bp->fetch_hstop)
    {
        bp->active = 0;
    }
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