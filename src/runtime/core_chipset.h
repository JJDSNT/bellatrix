#ifndef BELLATRIX_RUNTIME_CORE_CHIPSET_H
#define BELLATRIX_RUNTIME_CORE_CHIPSET_H

#include <stdint.h>
#include <stdbool.h>

#include "machine/machine.h"

typedef struct RigelContext RigelContext;

typedef struct RuntimeCoreChipset {
    RigelContext     *rigel;
    BellatrixMachine *machine;
    uint64_t          local_cycles;
    bool              running;
} RuntimeCoreChipset;

bool core_chipset_init(RuntimeCoreChipset *core,
                       RigelContext *rigel,
                       BellatrixMachine *machine);

void core_chipset_shutdown(RuntimeCoreChipset *core);
void core_chipset_reset(RuntimeCoreChipset *core);

#endif
