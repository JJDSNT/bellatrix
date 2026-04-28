// src/chipset/denise/denise.h

#ifndef BELLATRIX_CHIPSET_DENISE_H
#define BELLATRIX_CHIPSET_DENISE_H

#include <stdint.h>

#include "chipset/agnus/bitplanes.h"

/* Forward declare to avoid circular include */
typedef struct AgnusState AgnusState;

/* ------------------------------------------------------------------------- */
/* Register offsets                                                          */
/* ------------------------------------------------------------------------- */

#define DENISE_BPLCON0    0x0100u
#define DENISE_BPLCON1    0x0102u
#define DENISE_BPLCON2    0x0104u

#define DENISE_COLOR_BASE 0x0180u
#define DENISE_COLOR_END  0x01BEu

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

/*
 * Denise is a consumer/compositor:
 *
 * - Agnus fetches bitplanes
 * - Denise interprets them and writes pixels
 *
 * Denise does not own DMA timing and does not access chip RAM directly in the
 * current Bellatrix design.
 */
typedef struct Denise
{
    /*
     * Bitplane control registers
     */
    uint16_t bplcon0;
    uint16_t bplcon1;
    uint16_t bplcon2;

    /*
     * 32 Amiga color registers, preconverted to LE16 RGB565 for fast output.
     */
    uint16_t palette[32];

    /*
     * Attached Agnus reference (read-only during rendering).
     */
    const AgnusState *agnus;

} Denise;

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void denise_init(Denise *d);
void denise_reset(Denise *d);

/*
 * Denise consumes state; it does not generate time on its own.
 * Currently a no-op, kept for symmetry with the rest of the chipset.
 */
void denise_step(Denise *d, uint32_t ticks);

/* ------------------------------------------------------------------------- */
/* Wiring                                                                    */
/* ------------------------------------------------------------------------- */

void denise_attach_agnus(Denise *d, const AgnusState *agnus);

/* ------------------------------------------------------------------------- */
/* Bus protocol                                                              */
/* ------------------------------------------------------------------------- */

int      denise_handles_read(const Denise *d, uint32_t addr);
int      denise_handles_write(const Denise *d, uint32_t addr);
uint32_t denise_read(Denise *d, uint32_t addr, unsigned int size);
void     denise_write(Denise *d, uint32_t addr, uint32_t value, unsigned int size);

/* ------------------------------------------------------------------------- */
/* Low-level register write                                                  */
/* ------------------------------------------------------------------------- */

/*
 * Used by Agnus/Copper when routing custom register writes to Denise.
 */
void denise_write_reg(Denise *d, uint16_t reg, uint16_t value);

/* ------------------------------------------------------------------------- */
/* Render                                                                    */
/* ------------------------------------------------------------------------- */

/*
 * Render one scanline from pre-fetched bitplane data.
 *
 * line_idx and vheight are derived from agnus->diwstrt/diwstop and
 * bp->line_vpos internally, so the caller does not need to track them.
 */
void denise_render_line(Denise *d, const AgnusState *agnus,
                        const BitplaneState *bp);

#endif /* BELLATRIX_CHIPSET_DENISE_H */