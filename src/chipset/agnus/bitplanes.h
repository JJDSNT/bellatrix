#pragma once

#include <stdint.h>

struct AgnusState;

/*
 * Estado de fetch de bitplanes pertencente ao Agnus.
 *
 * O Agnus é dono:
 * - do timing/fetch DMA
 * - dos ponteiros correntes
 * - dos modulos
 * - do line buffer resultante
 *
 * O Denise consome o buffer já fetchado.
 */
typedef struct BitplaneState
{
    int active;
    int line_ready;

    int hires;
    int nplanes;

    int ddf_words;
    int line_words_fetched;

    int line_vpos;
    int fetch_hstart;
    int fetch_hstop;

    uint32_t cur_bplpt[6];

    /*
     * Words planar da linha corrente.
     * [plane][word]
     *
     * 80 words cobre o máximo de DDF words para PAL lores.
     */
    uint16_t line_words[6][80];
} BitplaneState;

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void bitplanes_init(BitplaneState *bp);
void bitplanes_reset(BitplaneState *bp);

/* ------------------------------------------------------------------------- */
/* frame/line flow                                                           */
/* ------------------------------------------------------------------------- */

void bitplanes_begin_frame(BitplaneState *bp, const struct AgnusState *agnus,
                           int nplanes, int hires);
void bitplanes_fetch_line(BitplaneState *bp, const struct AgnusState *agnus,
                          int vpos_abs);
void bitplanes_step(BitplaneState *bp, struct AgnusState *agnus);
void bitplanes_end_line(BitplaneState *bp, struct AgnusState *agnus);

/* ------------------------------------------------------------------------- */
/* helpers for Denise                                                        */
/* ------------------------------------------------------------------------- */

int              bitplanes_line_ready(const BitplaneState *bp);
void             bitplanes_clear_line_ready(BitplaneState *bp);
int              bitplanes_nplanes(const BitplaneState *bp);
int              bitplanes_ddf_words(const BitplaneState *bp);
const uint16_t  *bitplanes_plane_words(const BitplaneState *bp, int plane);
int              bitplanes_line_vpos(const BitplaneState *bp);
