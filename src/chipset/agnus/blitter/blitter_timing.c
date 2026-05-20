#include "blitter.h"

#include <string.h>

void blitter_begin_command(BlitterState *b)
{
    blitter_build_command(b);
    blitter_start_timing(b);
}

void blitter_publish_result(BlitterState *b)
{
    b->regs.bltapt = b->result.final_apt;
    b->regs.bltbpt = b->result.final_bpt;
    b->regs.bltcpt = b->result.final_cpt;
    b->regs.bltdpt = b->result.final_dpt;

    b->regs.bltadat = b->result.final_adat;
    b->regs.bltbdat = b->result.final_bdat;
    b->regs.bltcdat = b->result.final_cdat;
    b->regs.bltddat = b->result.final_ddat;

    /*
     * A completed operation consumes its latched size.
     * Keeping the raw BLTSIZE write is useful for MMIO reads,
     * but the expanded internal dimensions are no longer active.
     */
    b->regs.bltsizh = 0;
    b->regs.bltsizv = 0;
}

uint32_t blitter_estimate_cycles(const BlitCommand *cmd)
{
    /*
     * Very rough initial estimation.
     *
     * This is NOT cycle exact.
     * It only provides observable timing
     * coherent enough for early Bellatrix stages.
     *
     * Future versions can model:
     *   - DMA slot ownership
     *   - bus contention
     *   - nasty mode
     *   - line mode timing
     *   - fill mode timing
     */

    uint32_t words =
        (uint32_t)cmd->width_words *
        (uint32_t)cmd->height_lines;

    if (words == 0) {
        words = 1;
    }

    return words;
}

void blitter_start_timing(BlitterState *b)
{
    b->busy = true;

    b->exec_state = BLITTER_EXEC_PENDING;

    memset(&b->result, 0, sizeof(b->result));
    b->dma_slots_consumed = 0;
    b->cycles_remaining =
        blitter_estimate_cycles(&b->cmd);

    /*
     * Hardware starts with BLTZERO set.
     * It is cleared only if a non-zero
     * word is generated.
     */

    b->result.zero = true;
}

void blitter_force_finish(
    BlitterState *b,
    BlitterHost host
) {
    b->busy = false;

    b->exec_state = BLITTER_EXEC_IDLE;

    b->cycles_remaining = 0;

    /*
     * Publish BLIT interrupt through Paula.
     */

    if (host.intreq) {
        host.intreq(
            host.opaque,
            BLITTER_INTREQ_BLIT
        );
    }
}
