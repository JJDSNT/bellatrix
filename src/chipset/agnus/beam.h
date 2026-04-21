#pragma once

#include <stdint.h>

typedef struct BeamState
{
    uint32_t hpos;
    uint32_t vpos;
    uint32_t frame;

    /*
     * Interlace / long-frame
     */
    uint8_t lof;
    uint8_t lof_toggle;

    /*
     * Long-line
     */
    uint8_t lol;
    uint8_t lol_toggle;

    /*
     * Latched state for edge detection
     */
    uint8_t in_vblank;
} BeamState;

/* ------------------------------------------------------------------------- */
/* PAL defaults                                                              */
/* ------------------------------------------------------------------------- */

#define BEAM_PAL_LINES 313u
#define BEAM_PAL_HPOS  454u

/*
 * VBL model:
 * start at line 0 of new frame, end at first visible line.
 *
 * This is intentionally simple and good enough for current Bellatrix stage.
 */
#define BEAM_PAL_VBL_START 0u
#define BEAM_PAL_VBL_END   25u

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void beam_init(BeamState *b);
void beam_reset(BeamState *b);

/* ------------------------------------------------------------------------- */
/* stepping                                                                   */
/* ------------------------------------------------------------------------- */

void beam_step(BeamState *b, uint64_t ticks);
void beam_eol(BeamState *b);
void beam_eof(BeamState *b);

/* ------------------------------------------------------------------------- */
/* queries                                                                    */
/* ------------------------------------------------------------------------- */

int beam_is_in_vblank(const BeamState *b);
int beam_vblank_entered(BeamState *b);
int beam_vblank_exited(BeamState *b);