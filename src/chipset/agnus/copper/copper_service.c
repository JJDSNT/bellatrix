// src/chipset/agnus/copper/copper_service.c

//
// Copper service — temporal scheduling and wake-up logic.
// The Copper core executes instructions; this file decides when it may run.
//

#include "copper_service.h"

#include "copper.h"

#include "../agnus.h"
#include "../beam.h"
#include "../blitter.h"
#include "support.h"

/* ------------------------------------------------------------------------- */
/* local constants                                                           */
/* ------------------------------------------------------------------------- */

#define CHIP_RAM_MASK 0x001FFFFFu

/*
 * Small deterministic budget used when the Copper wakes at a beam position.
 *
 * The important rule is:
 * after a WAIT becomes true, the Copper must be allowed to execute the
 * immediately pending MOVE(s) before bitplanes snapshot the line state.
 */
#define COPPER_WAKE_BUDGET 8

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

static inline uint16_t copper_service_vhpos(const AgnusState *agnus)
{
    return (uint16_t)(((agnus->beam.vpos & 0xFFu) << 8) |
                      (agnus->beam.hpos & 0xFEu));
}

static inline int copper_service_wait_match(uint16_t waitpos,
                                            uint16_t waitmask,
                                            uint16_t vhpos)
{
    uint8_t vmask = (waitmask >> 8) & 0xFFu;
    uint8_t vwait = (waitpos >> 8) & 0xFFu;
    uint8_t vcur  = (vhpos >> 8) & 0xFFu;

    if ((vcur & vmask) > (vwait & vmask))
        return 1;

    if ((vcur & vmask) < (vwait & vmask))
        return 0;

    uint8_t hmask = waitmask & 0xFEu;
    uint8_t hwait = waitpos  & 0xFEu;
    uint8_t hcur  = vhpos    & 0xFEu;

    uint8_t hcmp = (hcur < 0xE0u)
        ? ((hcur + 2u) & 0xFEu)
        : ((hcur - 0xE0u) & 0xFEu);

    return ((hcmp & hmask) >= (hwait & hmask)) ? 1 : 0;
}

static inline int copper_service_is_waiting(const CopperState *c)
{
    return c->state == COPPER_STATE_WAITING_RASTER ||
           c->state == COPPER_STATE_WAITING_BLITTER;
}

/* ------------------------------------------------------------------------- */
/* public API                                                                */
/* ------------------------------------------------------------------------- */

void copper_service_init(CopperService *svc)
{
    svc->enabled = 1;
    svc->wake_budget = COPPER_WAKE_BUDGET;
}

void copper_service_reset(CopperService *svc)
{
    copper_service_init(svc);
}

void copper_service_vbl_reload(CopperService *svc, CopperState *c)
{
    if (!svc->enabled)
        return;

    if ((c->cop1lc & CHIP_RAM_MASK) == 0)
    {
        kprintf("[COPPER] vbl_reload skipped — cop1lc=0\n");
        c->state = COPPER_STATE_HALTED;
        return;
    }

    copper_restart_program(c, c->cop1lc);
    c->after_vbl_reload = 1;

    kprintf("[COPPER] vbl_reload cop1lc=0x%06x pc=0x%05x\n",
            (unsigned)(c->cop1lc & CHIP_RAM_MASK),
            (unsigned)c->pc);
}

void copper_service_step(CopperService *svc,
                         CopperState *c,
                         AgnusState *agnus,
                         uint64_t budget)
{
    if (!svc->enabled)
        return;

    if (budget == 0)
        return;

    /*
     * Normal execution path.
     *
     * This is intentionally not beam-aware by itself. Beam ownership remains
     * with Agnus; this service is called from the Agnus step/order point that
     * is safe for Copper side effects.
     */
    copper_step_exec(c, agnus, (int)budget);
}

void copper_service_poll(CopperService *svc,
                         CopperState *c,
                         AgnusState *agnus)
{
    if (!svc->enabled)
        return;

    if (!copper_service_is_waiting(c))
        return;

    const uint16_t vh = copper_service_vhpos(agnus);

    if (c->state == COPPER_STATE_WAITING_BLITTER)
    {
        if (blitter_is_busy(&agnus->blitter))
            return;

        if (!copper_service_wait_match(c->waitpos, c->waitmask, vh))
        {
            kprintf("[COPPER] blitter wake pc=%05x vhpos=%04x -> WAIT_RASTER\n",
                    (unsigned)c->pc,
                    (unsigned)vh);
            c->state = COPPER_STATE_WAITING_RASTER;
            return;
        }

        kprintf("[COPPER] blitter wake pc=%05x vhpos=%04x -> PASS\n",
                (unsigned)c->pc,
                (unsigned)vh);

        c->state = COPPER_STATE_FETCH_IR1;
        copper_step_exec(c, agnus, (int)svc->wake_budget);
        return;
    }

    if (c->state == COPPER_STATE_WAITING_RASTER)
    {
        if (!copper_service_wait_match(c->waitpos, c->waitmask, vh))
            return;

        kprintf("[COPPER] raster wake pc=%05x vhpos=%04x\n",
                (unsigned)c->pc,
                (unsigned)vh);

        c->state = COPPER_STATE_FETCH_IR1;

        /*
         * This is the essential fix:
         *
         * Do not merely wake the Copper and return. Run a small immediate
         * budget so the MOVE after the WAIT lands before bitplanes/Denise
         * snapshot the line state.
         */
        copper_step_exec(c, agnus, (int)svc->wake_budget);
    }
}
