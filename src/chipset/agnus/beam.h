#pragma once

#include <stdint.h>

/* ------------------------------------------------------------------------- */
/* Beam state                                                                */
/* ------------------------------------------------------------------------- */

typedef struct BeamState
{
    /*
     * Current raster position.
     *
     * hpos: 0 .. line_hmax-1
     * vpos: 0 .. frame_vmax-1
     */
    uint32_t hpos;
    uint32_t vpos;

    /*
     * Frame counter.
     *
     * 64-bit to avoid early wrap during long debug sessions.
     */
    uint64_t frame;

    /*
     * Long-frame (interlace-related behavior).
     */
    uint8_t lof;
    uint8_t lof_toggle;

    /*
     * Long-line behavior.
     */
    uint8_t lol;
    uint8_t lol_toggle;

    /*
     * Latched VBL state for edge detection.
     */
    uint8_t in_vblank;

} BeamState;

/* ------------------------------------------------------------------------- */
/* PAL defaults                                                              */
/* ------------------------------------------------------------------------- */

/*
 * These are counts (NOT max indices):
 *
 *   hpos runs from 0 to BEAM_PAL_HPOS - 1
 *   vpos runs from 0 to BEAM_PAL_LINES - 1
 */
#define BEAM_PAL_LINES 313u
#define BEAM_PAL_HPOS  454u

/*
 * Simplified VBL model:
 *
 * Frame starts inside VBL (line 0)
 * Leaves VBL at first visible line
 */
#define BEAM_PAL_VBL_START 0u
#define BEAM_PAL_VBL_END   25u

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void beam_init(BeamState *b);
void beam_reset(BeamState *b);

/* ------------------------------------------------------------------------- */
/* stepping                                                                  */
/* ------------------------------------------------------------------------- */

/*
 * Advance beam by N cycles.
 */
void beam_step(BeamState *b, uint64_t ticks);

/*
 * Advance to next line.
 */
void beam_eol(BeamState *b);

/*
 * Advance to next frame.
 */
void beam_eof(BeamState *b);

/* ------------------------------------------------------------------------- */
/* queries                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Current VBL state (derived).
 */
int beam_is_in_vblank(const BeamState *b);

/*
 * Edge detection helpers (stateful).
 */
int beam_vblank_entered(BeamState *b);
int beam_vblank_exited(BeamState *b);

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Current raster position.
 */
uint32_t beam_hpos(const BeamState *b);
uint32_t beam_vpos(const BeamState *b);
uint64_t beam_frame(const BeamState *b);

/*
 * Current dynamic limits (considering toggles).
 */
uint32_t beam_line_hmax(const BeamState *b);
uint32_t beam_frame_vmax(const BeamState *b);