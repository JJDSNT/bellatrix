// src/chipset/denise/denise.h

#ifndef BELLATRIX_CHIPSET_DENISE_H
#define BELLATRIX_CHIPSET_DENISE_H

#include <stdint.h>

#include "chipset/agnus/bitplanes.h"

/* Forward declare to avoid circular include */
typedef struct AgnusState AgnusState;

/* ---------------------------------------------------------------------------
 * Register offsets (decoded, same convention as agnus.h)
 * ------------------------------------------------------------------------- */

#define DENISE_BPLCON0   0x0100u
#define DENISE_BPLCON1   0x0102u
#define DENISE_BPLCON2   0x0104u

#define DENISE_COLOR_BASE 0x0180u
#define DENISE_COLOR_END  0x01BEu

/* ---------------------------------------------------------------------------
 * State — owned entirely by Denise; no singletons
 * ------------------------------------------------------------------------- */

typedef struct Denise {
    uint16_t bplcon0;
    uint16_t bplcon1;
    uint16_t bplcon2;
    uint16_t palette[32];   /* pre-converted to LE16 RGB565 */

    const AgnusState *agnus; /* attached Agnus (read-only at render time) */
} Denise;

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

void denise_init(Denise *d);
void denise_reset(Denise *d);

/* time advance — no-op; Denise consumes state, does not generate time */
void denise_step(Denise *d, uint32_t ticks);

/* ---------------------------------------------------------------------------
 * Wiring
 * ------------------------------------------------------------------------- */

void denise_attach_agnus(Denise *d, const AgnusState *agnus);

/* ---------------------------------------------------------------------------
 * Bus protocol — called by machine.c read/write dispatch
 * ------------------------------------------------------------------------- */

int      denise_handles_read(const Denise *d, uint32_t addr);
int      denise_handles_write(const Denise *d, uint32_t addr);
uint32_t denise_read(Denise *d, uint32_t addr, unsigned int size);
void     denise_write(Denise *d, uint32_t addr, uint32_t value, unsigned int size);

/* ---------------------------------------------------------------------------
 * Low-level register write — used by Agnus/Copper when routing MMIO
 * ------------------------------------------------------------------------- */

void denise_write_reg(Denise *d, uint16_t reg, uint16_t value);

/* ---------------------------------------------------------------------------
 * Render
 * ------------------------------------------------------------------------- */

/*
 * Render one scanline from pre-fetched bitplane data.
 * Called by Agnus per line after bitplanes_fetch_line.
 *
 * line_idx: 0-based line index within the display window
 * vheight:  total display window height in lines (for framebuffer centering)
 */
void denise_render_line(Denise *d, const BitplaneState *bp,
                        int line_idx, int vheight);

#endif /* BELLATRIX_CHIPSET_DENISE_H */
