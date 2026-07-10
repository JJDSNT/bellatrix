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

/* Snapshot of the CPU-published target and Core 2 drained chipset time.
 * Returns false when the separate chipset core is not active/available. */
bool core_chipset_get_progress(uint64_t *chipset_cck, uint64_t *target_cck);

/* Latest IPL published by the chipset core.  The CPU core consumes this at a
 * backend boundary; Core 2 must never mutate Musashi/Emu68 CPU state itself. */
uint8_t core_chipset_get_pending_ipl(void);
void core_chipset_set_pending_ipl(uint8_t ipl);

/* MMIO-critical barrier: block the calling (CPU) core until the chipset core
 * has advanced to the CPU's published time, so a critical register access sees
 * fresh chipset state instead of state up to CHIPSET_MAX_BACKLOG_CCK stale.
 * No-op when multicore is disabled or the chipset core is not running. */
void core_chipset_wait_caught_up(void);

#endif
