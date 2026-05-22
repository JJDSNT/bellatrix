#ifndef BELLATRIX_RUNTIME_CORE_GFX_H
#define BELLATRIX_RUNTIME_CORE_GFX_H

#include <stdint.h>
#include <stdbool.h>

#include "machine/machine.h"

typedef struct RuntimeCoreGFX {
    BellatrixMachine *machine;

    bool running;

    /*
     * Agnus owns observable time.
     */
    uint64_t master_cycles;

    /*
     * Beam position snapshot.
     */
    uint16_t vpos;
    uint16_t hpos;

    /*
     * Frame accounting.
     */
    uint64_t frame_counter;
} RuntimeCoreGFX;

bool core_gfx_init(RuntimeCoreGFX *core,
                   BellatrixMachine *machine);

void core_gfx_shutdown(RuntimeCoreGFX *core);

void core_gfx_step(RuntimeCoreGFX *core,
                   uint32_t cycles);

uint64_t core_gfx_master_cycles(
    const RuntimeCoreGFX *core);

#endif
