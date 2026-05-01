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
    /*
     * Linha ativa para fetch
     */
    int active;

    /*
     * Linha completamente pronta para consumo pelo Denise
     */
    int line_ready;

    /*
     * Estado de display latched por linha
     */
    int hires;
    int nplanes;

    /*
     * Quantidade total de words esperadas pela linha (DDF)
     * e quantas já foram efetivamente fetchadas.
     */
    int ddf_words;
    int line_words_fetched;

    /*
     * Índice incremental de fetch dentro da linha.
     * Vai de 0 até ddf_words.
     */
    int fetch_index;

    /*
     * Linha atual e intervalo horizontal de fetch.
     */
    int line_vpos;
    int fetch_hstart;
    int fetch_hstop;

    /*
     * Ponteiros correntes de fetch por plano.
     * Esses ponteiros são latched no começo da linha e vão avançando
     * progressivamente conforme os words são buscados.
     */
    uint32_t cur_bplpt[6];

    /*
     * Words planar da linha corrente.
     * [plane][word]
     *
     * 80 words cobre o máximo de DDF words usado pelo modelo atual.
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

void bitplanes_begin_line(BitplaneState *bp, const struct AgnusState *agnus,
                          int vpos_abs);

/*
 * Completa o fetch da linha atual.
 * Mantida por compatibilidade; o modelo principal agora é incremental.
 */
void bitplanes_fetch_line(BitplaneState *bp, struct AgnusState *agnus,
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