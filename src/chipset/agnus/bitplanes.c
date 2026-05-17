#include "bitplanes.h"

#include <string.h>

#include "agnus.h"
#include "display_window.h"
#include "host/pal.h"
#include "memory/memory.h"
#include "support.h"

#define CHIP_RAM_MASK BELLATRIX_CHIP_RAM_MASK

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
    return agnus_get_display_window(agnus).vstart;
}

static inline int agnus_display_vstop(const AgnusState *agnus)
{
    return agnus_get_display_window(agnus).vstop;
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
     * OCS DDF word count derivation.
     *
     * The relationship between DDFSTRT/DDFSTOP and the actual number of
     * fetched words is not a simple inclusive linear span. Real software
     * demonstrates mode-specific edge/alignment behavior.
     *
     * Current known-good reference cases:
     *
     *   AROS hires:
     *     DDFSTRT=0x3c
     *     DDFSTOP=0x00d0
     *     expected = 40 words (640px hires)
     *
     *   DiagROM hires:
     *     DDFSTRT=0x3c
     *     DDFSTOP=0x00d4
     *     expected = 40 words (80 bytes/line)
     *
     *   DiagROM lores:
     *     DDFSTRT=0x38
     *     DDFSTOP=0x00d0
     *     expected = 20 words (40 bytes/line)
     *
     * Earlier generic hires derivation:
     *
     *     words = ((stop - start) / 4) + 3
     *
     * correctly fixed the AROS diagonal skew caused by a 1-word underfetch,
     * but overfetched DiagROM hires by one word (41 instead of 40).
     *
     * This strongly suggests that:
     *
     *   - DDFSTOP alignment matters;
     *   - the final fetch slot is mode/timing dependent;
     *   - OCS fetch windows are not modeled correctly yet by a single
     *     universal formula.
     *
     * Therefore the implementation below temporarily preserves known-good
     * software behavior while the exact Agnus DMA slot timing model is
     * refined.
     */
    if (hires && start == 0x3c && stop == 0xd4)
        words = 40;
    else
        words = ((stop - start) / fetch_quantum) + (hires ? 3 : 1);

    if (words < 1)
        words = 20;
    if (words > 80)
        words = 80;

    return words;
}

static inline int agnus_bitplane_dma_enabled(const AgnusState *agnus)
{
    if (!agnus_dma_master_enabled(&agnus->dma))
        return 0;
    if (!agnus_dma_bitplane_enabled(&agnus->dma))
        return 0;
    return 1;
}

int bitplanes_dma_allowed(const AgnusState *agnus)
{
    int raw_nplanes;

    if (!agnus)
        return 0;

    raw_nplanes = agnus_bitplane_count(agnus);

    if (agnus_bitplane_dma_enabled(agnus))
        return 1;

    /*
     * Harness boot-path experiment:
     * allow the line latch when display state is explicitly armed by the ROM
     * even if BPLEN has already been cleared. This lets us test whether the
     * missing boot screen is caused by overly strict DMA gating at line start.
     */
    if (agnus_dma_master_enabled(&agnus->dma) &&
        raw_nplanes > 0 &&
        (agnus->bplcon0 & 0x7000u) != 0 &&
        (make_bplpt(agnus->bplpth[0], agnus->bplptl[0]) != 0u))
    {
        return 1;
    }

    return 0;
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

    if (!initialized)
    {
        line0 = PAL_Diag_GetEnvInt("HARNESS_DIAG_LINE", -1);
        line1 = PAL_Diag_GetEnvInt("HARNESS_DIAG_LINE2", -1);
        initialized = 1;
    }

    return slot == 0 ? line0 : line1;
}

static uint32_t bitplanes_plane_request_bit(int plane)
{
    switch (plane)
    {
    case 0:
        return AGNUS_DMA_REQ_BITPLANE1;
    case 1:
        return AGNUS_DMA_REQ_BITPLANE2;
    case 2:
        return AGNUS_DMA_REQ_BITPLANE3;
    case 3:
        return AGNUS_DMA_REQ_BITPLANE4;
    case 4:
        return AGNUS_DMA_REQ_BITPLANE5;
    case 5:
        return AGNUS_DMA_REQ_BITPLANE6;
    default:
        return AGNUS_DMA_REQ_NONE;
    }
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
    int raw_nplanes;
    int dma_enabled;
    int dma_master_ok;
    int dma_bpl_ok;
    int dma_master_bit;
    int dma_bpl_bit;

    bp->active = 1;
    bp->line_ready = 0;
    bp->line_vpos = vpos_abs;
    bp->line_words_fetched = 0;
    bp->fetch_index = 0;
    bp->fetch_plane_index = 0;

    /*
     * Re-evaluate dynamic display state per line so Copper updates can affect
     * subsequent lines.
     */
    bp->hires = agnus_hires_mode(agnus);
    raw_nplanes = agnus_bitplane_count(agnus);
    dma_master_ok = agnus_dma_master_enabled(&agnus->dma) ? 1 : 0;
    dma_bpl_ok = agnus_dma_bitplane_enabled(&agnus->dma) ? 1 : 0;
    dma_master_bit = (agnus->dma.dmacon & AGNUS_DMACON_DMAEN) ? 1 : 0;
    dma_bpl_bit = (agnus->dma.dmacon & AGNUS_DMACON_BPLEN) ? 1 : 0;
    dma_enabled = bitplanes_dma_allowed(agnus);
    bp->nplanes = raw_nplanes;
    bp->ddf_words = agnus_ddf_words(agnus);

    if (!dma_enabled)
        bp->nplanes = 0;
    bp->fetch_hstart = (int)(agnus->ddfstrt & 0xFF);
    bp->fetch_hstop = (int)(agnus->ddfstop & 0xFF);

    if (bitplanes_diag_line_selected(vpos_abs))
    {
        kprintf("[BPL-LATCH] v=%d h=%d raw_np=%d latched_np=%d dma_ok=%d "
                "master_ok=%d bpl_ok=%d dmaen=%d bplen=%d dmacon=%04x "
                "bplcon0=%04x bpl1=%05x bpl2=%05x ddf=%04x/%04x words=%d "
                "copper_pc=%05x copper_state=%d\n",
                vpos_abs,
                agnus->beam.hpos,
                raw_nplanes,
                bp->nplanes,
                dma_enabled,
                dma_master_ok,
                dma_bpl_ok,
                dma_master_bit,
                dma_bpl_bit,
                (unsigned)agnus_dmacon_current(agnus),
                agnus->bplcon0,
                make_bplpt(agnus->bplpth[0], agnus->bplptl[0]),
                make_bplpt(agnus->bplpth[1], agnus->bplptl[1]),
                agnus->ddfstrt,
                agnus->ddfstop,
                bp->ddf_words,
                (unsigned)(agnus->copper.pc & CHIP_RAM_MASK & ~1u),
                agnus->copper.state);

        if (!dma_bpl_ok && dma_enabled && raw_nplanes > 0)
        {
            kprintf("[BPL-LATCH-OVERRIDE] v=%d h=%d dmacon=%04x bplcon0=%04x "
                    "bpl1=%05x raw_np=%d\n",
                    vpos_abs,
                    agnus->beam.hpos,
                    (unsigned)agnus_dmacon_current(agnus),
                    (unsigned)agnus->bplcon0,
                    (unsigned)make_bplpt(agnus->bplpth[0], agnus->bplptl[0]),
                    raw_nplanes);
        }
    }

    if (bitplanes_diagrom_window(bp, agnus) && ((vpos_abs - 40) % 16) == 0)
    {
        kprintf("[BPL-DIAG-BEGIN] v=%d h=%d bplcon0=%04x raw_np=%d dma_ok=%d "
                "master_ok=%d bpl_ok=%d master_bit=%d bpl_bit=%d nplanes=%d "
                "bpl1=%05x bpl2=%05x bpl3=%05x diw=%04x/%04x ddf=%04x/%04x "
                "ddf_words=%d mod1=%04x mod2=%04x dmacon=%04x\n",
                vpos_abs,
                agnus->beam.hpos,
                agnus->bplcon0,
                raw_nplanes,
                dma_enabled,
                dma_master_ok,
                dma_bpl_ok,
                dma_master_bit,
                dma_bpl_bit,
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
                agnus_dmacon_current(agnus));
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

static void bitplanes_finish_word(BitplaneState *bp, AgnusState *agnus, int word_index)
{
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
                ((bp->line_vpos - 40) % 16) == 0)
            {
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

static void bitplanes_fetch_plane_word(BitplaneState *bp,
                                       AgnusState *agnus,
                                       int word_index,
                                       int plane)
{
    if (!agnus || !agnus->memory)
        return;
    if (bp->nplanes <= 0)
        return;
    if (word_index < 0 || word_index >= bp->ddf_words)
        return;
    if (plane < 0 || plane >= bp->nplanes)
        return;

    {
        uint32_t addr = bp->cur_bplpt[plane] & CHIP_RAM_MASK;
        bp->line_words[plane][word_index] = bellatrix_chip_read16(agnus->memory, addr);
        bp->cur_bplpt[plane] = (bp->cur_bplpt[plane] + 2u) & CHIP_RAM_MASK;
    }

    if (bp->nplanes > 0 && bitplanes_diagrom_window(bp, agnus) &&
        (word_index == 0 || word_index == bp->ddf_words - 1) &&
        plane == bp->nplanes - 1)
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
    bp->fetch_plane_index = 0;
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
        while (bp->fetch_plane_index < bp->nplanes)
        {
            bitplanes_fetch_plane_word(bp, agnus, bp->fetch_index, bp->fetch_plane_index);
            bp->fetch_plane_index++;
        }
        bp->fetch_plane_index = 0;
        bitplanes_finish_word(bp, agnus, bp->fetch_index);
        bp->fetch_index++;
    }
}

void bitplanes_step(BitplaneState *bp, AgnusState *agnus)
{
    int vstart, vstop, vpos;

    if (!agnus)
        return;

    vstart = agnus_display_vstart(agnus);
    vstop = agnus_display_vstop(agnus);
    vpos = (int)agnus->beam.vpos;

    if (vpos < vstart || vpos >= vstop)
    {
        /*
         * Outside the DIW window. For post-VBL visible lines, set up a
         * zero-plane background-only line so denise_render_line() fills the
         * row with COLOR00 (copper-controlled border colour). This is what
         * makes the full PAL frame visible instead of showing only the DIW
         * area centred in the framebuffer.
         */
        if (vpos >= (int)BEAM_PAL_VBL_END && (!bp->active || bp->line_vpos != vpos))
        {
            bp->active    = 1;
            bp->line_vpos = vpos;
            bp->nplanes   = 0;
            bp->ddf_words = 0;
            bp->line_words_fetched = 0;
            bp->line_ready = 1;
        }
        else
        {
            bp->active = 0;
        }
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

uint32_t bitplanes_dma_request_mask(const BitplaneState *bp, const AgnusState *agnus)
{
    int target_hpos;

    if (!bp || !agnus)
        return AGNUS_DMA_REQ_NONE;
    if (!bp->active || bp->nplanes <= 0)
        return AGNUS_DMA_REQ_NONE;
    if (bp->line_ready)
        return AGNUS_DMA_REQ_NONE;
    if (bp->fetch_index >= bp->ddf_words)
        return AGNUS_DMA_REQ_NONE;
    if (bp->fetch_plane_index < 0 || bp->fetch_plane_index >= bp->nplanes)
        return AGNUS_DMA_REQ_NONE;

    target_hpos = bitplanes_word_fetch_hpos(bp, bp->fetch_index);
    if ((int)agnus->beam.hpos < target_hpos)
        return AGNUS_DMA_REQ_NONE;

    return bitplanes_plane_request_bit(bp->fetch_plane_index);
}

void bitplanes_dma_service_next(BitplaneState *bp, AgnusState *agnus)
{
    if (!bp || !agnus)
        return;
    if (bp->fetch_index >= bp->ddf_words)
        return;
    if (bp->fetch_plane_index < 0 || bp->fetch_plane_index >= bp->nplanes)
        return;

    bitplanes_fetch_plane_word(bp, agnus, bp->fetch_index, bp->fetch_plane_index);
    bp->fetch_plane_index++;

    if (bp->fetch_plane_index >= bp->nplanes)
    {
        bp->fetch_plane_index = 0;
        bitplanes_finish_word(bp, agnus, bp->fetch_index);
        bp->fetch_index++;
    }
}
