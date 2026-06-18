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

/* Coarse spinlock guarding chipset state from concurrent CPU-side bus
 * access. Held by the CPU core (Emu68 fault path and Musashi bridge calls)
 * while dispatching MMIO, and not needed by the chipset core itself (Rigel
 * stepping is only ever driven from one core). No-op when multicore is
 * disabled. */
void core_chipset_lock_acquire(void);
void core_chipset_lock_release(void);

#endif
