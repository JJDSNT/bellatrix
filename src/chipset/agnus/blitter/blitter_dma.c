#include "blitter.h"

void blitter_step_dma(
    BlitterState *b,
    BlitterMemory mem,
    BlitterHost host,
    uint32_t dma_slots
) {
    /*
     * No active blitter operation.
     */

    if (!b->busy) {
        return;
    }

    /*
     * First DMA opportunity:
     * execute the semantic backend.
     *
     * The result becomes internally available
     * immediately, but externally the blitter
     * remains busy until the estimated DMA
     * duration elapses.
     *
     * This preserves the useful legacy intuition
     * behind "dma_service_grant()":
     * each DMA grant advances observable progress,
     * but semantic execution and observable completion
     * are intentionally not the same event anymore.
     */

    if (b->exec_state == BLITTER_EXEC_PENDING) {

        bool ok =
            blitter_execute_reference(
                b,
                mem
            );

        if (!ok) {

            /*
             * Abort operation.
             */

            b->busy = false;

            b->exec_state = BLITTER_EXEC_IDLE;

            b->cycles_remaining = 0;

            return;
        }

        b->exec_state = BLITTER_EXEC_RUNNING;
    }

    /*
     * Consume DMA time.
     *
     * The old model of "one grant decrements the remaining
     * blit budget until completion" is still relevant here.
     * What changed is only the separation of concerns:
     * DMA owns temporal progress, while the reference backend
     * owns the computed result.
     */

    b->dma_slots_consumed += dma_slots;

    if (b->cycles_remaining > dma_slots) {

        b->cycles_remaining -= dma_slots;

        return;
    }

    /*
     * Operation becomes externally visible
     * as complete.
     */

    b->cycles_remaining = 0;
    blitter_publish_result(b);

    blitter_force_finish(
        b,
        host
    );
}
