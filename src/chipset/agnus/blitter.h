#ifndef BELLATRIX_CHIPSET_AGNUS_BLITTER_H
#define BELLATRIX_CHIPSET_AGNUS_BLITTER_H

#include <stdint.h>

struct AgnusState;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

typedef struct BlitterState {
    uint16_t bltcon0;
    uint16_t bltcon1;

    uint16_t bltsize;

    uint8_t  busy;
    uint32_t cycles_remaining;
} BlitterState;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void blitter_init(BlitterState *b);

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

void blitter_step(BlitterState *b, struct AgnusState *agnus, uint64_t ticks);
int blitter_is_busy(const BlitterState *b);

// ---------------------------------------------------------------------------
// MMIO (register already decoded by bus/router/Agnus)
// ---------------------------------------------------------------------------

void blitter_write_reg(BlitterState *b, struct AgnusState *agnus, uint16_t reg, uint16_t value);

#endif