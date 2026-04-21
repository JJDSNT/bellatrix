#include "beam.h"

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

    b->in_vblank = 1;
}

void beam_reset(BeamState *b)
{
    beam_init(b);
}

/* ------------------------------------------------------------------------- */
/* internal helpers                                                          */
/* ------------------------------------------------------------------------- */

static inline uint32_t beam_line_hmax(const BeamState *b)
{
    /*
     * PAL default line length with optional long-line toggle.
     */
    if (b->lol)
        return BEAM_PAL_HPOS + 1u;

    return BEAM_PAL_HPOS;
}

static inline uint32_t beam_frame_vmax(const BeamState *b)
{
    /*
     * PAL default frame height with optional long-frame toggle.
     */
    if (b->lof)
        return BEAM_PAL_LINES + 1u;

    return BEAM_PAL_LINES;
}

/* ------------------------------------------------------------------------- */
/* stepping                                                                  */
/* ------------------------------------------------------------------------- */

void beam_eol(BeamState *b)
{
    b->hpos = 0;
    b->vpos++;

    if (b->lol_toggle)
        b->lol ^= 1;
}

void beam_eof(BeamState *b)
{
    b->vpos = 0;
    b->hpos = 0;
    b->frame++;

    if (b->lof_toggle)
        b->lof ^= 1;

    if (!b->lol_toggle)
        b->lol = 0;
}

void beam_step(BeamState *b, uint64_t ticks)
{
    while (ticks--) {
        b->hpos++;

        if (b->hpos >= beam_line_hmax(b)) {
            beam_eol(b);

            if (b->vpos >= beam_frame_vmax(b)) {
                beam_eof(b);
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* queries                                                                   */
/* ------------------------------------------------------------------------- */

int beam_is_in_vblank(const BeamState *b)
{
    /* BEAM_PAL_VBL_START == 0; vpos is unsigned so lower bound always holds */
    return (b->vpos < BEAM_PAL_VBL_END) ? 1 : 0;
}

int beam_vblank_entered(BeamState *b)
{
    int now = beam_is_in_vblank(b);

    if (!b->in_vblank && now) {
        b->in_vblank = 1;
        return 1;
    }

    return 0;
}

int beam_vblank_exited(BeamState *b)
{
    int now = beam_is_in_vblank(b);

    if (b->in_vblank && !now) {
        b->in_vblank = 0;
        return 1;
    }

    return 0;
}