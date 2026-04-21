#include "bitplanes.h"

#include <string.h>

#include "agnus.h"
#include "memory/memory.h"
#include "support.h"

#define CHIP_RAM_MASK  0x001FFFFFu

#ifndef DMAF_BPLEN
#define DMAF_BPLEN (1u << 8)
#endif

/* ---------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

static inline uint32_t make_bplpt(uint16_t hi, uint16_t lo)
{
    return (((uint32_t)(hi & 0x001Fu) << 16) |
            ((uint32_t)(lo & 0xFFFEu))) & CHIP_RAM_MASK;
}

static inline int agnus_display_vstart(const AgnusState *agnus)
{
    return (agnus->diwstrt >> 8) & 0xFF;
}

static inline int agnus_display_vstop(const AgnusState *agnus)
{
    int vstart = agnus_display_vstart(agnus);
    int vstop  = (agnus->diwstop >> 8) & 0xFF;
    if (vstop <= vstart)
        vstop += 256;
    return vstop;
}

static inline int agnus_ddf_words(const AgnusState *agnus)
{
    int words = ((int)(agnus->ddfstop & 0xFE) - (int)(agnus->ddfstrt & 0xFC)) / 8 + 2;
    if (words < 1)  words = 20;
    if (words > 80) words = 80;
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

static void bitplanes_snapshot_base_ptrs(BitplaneState *bp, const AgnusState *agnus)
{
    for (int p = 0; p < 6; ++p)
        bp->cur_bplpt[p] = make_bplpt(agnus->bplpth[p], agnus->bplptl[p]);
}

static void bitplanes_apply_modulos(BitplaneState *bp, const AgnusState *agnus)
{
    /*
     * Odd planes (0, 2, 4) use BPL1MOD; even planes (1, 3, 5) use BPL2MOD.
     * Both are Agnus-owned registers.
     */
    for (int p = 0; p < bp->nplanes; ++p) {
        int16_t mod = (p & 1) ? agnus->bpl2mod : agnus->bpl1mod;
        bp->cur_bplpt[p] = (uint32_t)((int32_t)bp->cur_bplpt[p] + (int32_t)mod)
                           & CHIP_RAM_MASK;
    }
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

void bitplanes_init(BitplaneState *bp)
{
    memset(bp, 0, sizeof(*bp));
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
    bp->active             = 0;
    bp->line_ready         = 0;
    bp->hires              = hires;
    bp->nplanes            = (nplanes > 6) ? 6 : nplanes;
    bp->ddf_words          = agnus_ddf_words(agnus);
    bp->line_words_fetched = 0;
    bp->line_vpos          = -1;
    bp->fetch_hstart       = (int)(agnus->ddfstrt & 0xFF);
    bp->fetch_hstop        = (int)(agnus->ddfstop & 0xFF);

    bitplanes_snapshot_base_ptrs(bp, agnus);
}

void bitplanes_fetch_line(BitplaneState *bp, const AgnusState *agnus, int vpos_abs)
{
    int p, w;

    if (!agnus->memory)
        return;
    if (bp->nplanes <= 0)
        return;

    bp->line_vpos          = vpos_abs;
    bp->line_words_fetched = 0;
    bp->line_ready         = 0;

    for (p = 0; p < bp->nplanes; ++p) {
        for (w = 0; w < bp->ddf_words; ++w) {
            uint32_t addr = (bp->cur_bplpt[p] + (uint32_t)(w * 2)) & CHIP_RAM_MASK;
            bp->line_words[p][w] = bellatrix_chip_read16(agnus->memory, addr);
        }
        bp->cur_bplpt[p] = (bp->cur_bplpt[p] + (uint32_t)(bp->ddf_words * 2))
                           & CHIP_RAM_MASK;
    }

    bp->line_words_fetched = bp->ddf_words;
    bp->line_ready         = 1;

    bitplanes_apply_modulos(bp, agnus);
}

void bitplanes_step(BitplaneState *bp, AgnusState *agnus)
{
    int vstart, vstop, vpos, hpos;

    if (!agnus)
        return;

    if (!agnus_bitplane_dma_enabled(agnus))
        return;

    if (bp->nplanes <= 0)
        return;

    vstart = agnus_display_vstart(agnus);
    vstop  = agnus_display_vstop(agnus);
    vpos   = (int)agnus->beam.vpos;
    hpos   = (int)agnus->beam.hpos;

    if (vpos < vstart || vpos >= vstop) {
        bp->active = 0;
        return;
    }

    if (!bp->active || bp->line_vpos != vpos) {
        bp->active             = 1;
        bp->line_ready         = 0;
        bp->line_vpos          = vpos;
        bp->line_words_fetched = 0;
    }

    /*
     * Approximate fetch: consume entire line when beam reaches DDF start.
     * Not cycle-exact but architecturally correct — fetch stays in Agnus.
     */
    if (hpos >= bp->fetch_hstart && bp->line_words_fetched == 0 && agnus->memory) {
        bitplanes_fetch_line(bp, agnus, vpos);
    }

    if (bp->active && bp->line_words_fetched > 0 && hpos >= bp->fetch_hstop) {
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
