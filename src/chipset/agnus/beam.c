#include "beam.h"

/* ------------------------------------------------------------------------- */
/* internal helpers                                                          */
/* ------------------------------------------------------------------------- */

static inline int beam_compute_in_vblank(const BeamState *b)
{
    /*
     * BEAM_PAL_VBL_START == 0
     */
    return (b->vpos < BEAM_PAL_VBL_END) ? 1 : 0;
}

static inline void beam_sync_vblank_state(BeamState *b)
{
    b->in_vblank = beam_compute_in_vblank(b);
}

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void beam_init(BeamState *b)
{
    b->hpos = 0;
    b->vpos = 0;
    b->frame = 0;

    b->lof = 0;
    b->lof_toggle = 0;

    b->lol = 0;
    b->lol_toggle = 0;

    beam_sync_vblank_state(b);
}

void beam_reset(BeamState *b)
{
    beam_init(b);
}

/* ------------------------------------------------------------------------- */
/* helpers (public)                                                          */
/* ------------------------------------------------------------------------- */

uint32_t beam_line_hmax(const BeamState *b)
{
    return b->lol ? (BEAM_PAL_HPOS + 1u) : BEAM_PAL_HPOS;
}

uint32_t beam_frame_vmax(const BeamState *b)
{
    return b->lof ? (BEAM_PAL_LINES + 1u) : BEAM_PAL_LINES;
}

uint32_t beam_hpos(const BeamState *b)
{
    return b->hpos;
}

uint32_t beam_vpos(const BeamState *b)
{
    return b->vpos;
}

uint64_t beam_frame(const BeamState *b)
{
    return b->frame;
}

/* ------------------------------------------------------------------------- */
/* stepping                                                                  */
/* ------------------------------------------------------------------------- */

void beam_eof(BeamState *b)
{
    b->vpos = 0;
    b->hpos = 0;
    b->frame++;

    if (b->lof_toggle)
        b->lof ^= 1u;

    /*
     * If line toggle disabled, force lol = 0
     */
    if (!b->lol_toggle)
        b->lol = 0;

    beam_sync_vblank_state(b);
}

void beam_eol(BeamState *b)
{
    b->hpos = 0;
    b->vpos++;

    if (b->lol_toggle)
        b->lol ^= 1u;

    if (b->vpos >= beam_frame_vmax(b)) {
        beam_eof(b);
        return;
    }

    beam_sync_vblank_state(b);
}

void beam_step(BeamState *b, uint64_t ticks)
{
    while (ticks > 0) {

        uint32_t hmax = beam_line_hmax(b);
        uint32_t remaining = hmax - b->hpos;

        /*
         * Already at end of line
         */
        if (remaining == 0) {
            beam_eol(b);
            continue;
        }

        /*
         * Advance inside current line
         */
        if (ticks < (uint64_t)remaining) {
            b->hpos += (uint32_t)ticks;
            ticks = 0;
        } else {
            b->hpos += remaining;
            ticks -= remaining;
        }

        /*
         * End of line reached
         */
        if (b->hpos >= beam_line_hmax(b))
            beam_eol(b);
    }
}

/* ------------------------------------------------------------------------- */
/* queries                                                                   */
/* ------------------------------------------------------------------------- */

int beam_is_in_vblank(const BeamState *b)
{
    return beam_compute_in_vblank(b);
}

int beam_vblank_entered(BeamState *b)
{
    int old = b->in_vblank;
    int now = beam_compute_in_vblank(b);

    b->in_vblank = now;
    return (!old && now) ? 1 : 0;
}

int beam_vblank_exited(BeamState *b)
{
    int old = b->in_vblank;
    int now = beam_compute_in_vblank(b);

    b->in_vblank = now;
    return (old && !now) ? 1 : 0;
}