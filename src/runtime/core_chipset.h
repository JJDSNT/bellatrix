#ifndef BELLATRIX_RUNTIME_CORE_CHIPSET_H
#define BELLATRIX_RUNTIME_CORE_CHIPSET_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "machine/machine.h"
#include "runtime/timeline.h"

typedef struct RigelContext RigelContext;

typedef struct RuntimeCoreChipset {
    RigelContext     *rigel;
    BellatrixMachine *machine;
    uint64_t          local_cycles;
    _Atomic bool      running;
} RuntimeCoreChipset;

typedef struct RuntimeFrameCompletionStats {
    uint64_t produced;
    uint64_t presented;
    uint64_t coalesced;
} RuntimeFrameCompletionStats;

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

/* Hot read-only registers published by the Rigel owner and served lock-free
 * to CPU poll loops. DMACONR/INTENAR/INTREQR are scalar snapshots;
 * VPOSR/VHPOSR are derived from a coherent beam snapshot at CPU logical time.
 * publish must be called with the chipset access lock held; read returns false
 * when unavailable/unsupported so the caller falls back to rendezvous+lock. */
void core_chipset_publish_hot_regs(void);
bool core_chipset_read_hot_reg(uint32_t normalized_addr, uint32_t *value);

/* Posted-write queue: non-critical custom writes use CPU logical time in the
 * deterministic mode and current chipset time in self-paced modes. post
 * returns false when unavailable (fall back to sync); drain requires lock. */
bool core_chipset_post_write(uint32_t addr, uint32_t value, uint32_t size);
void core_chipset_drain_posted_writes(void);

/* Core 0 timeline authority. CPU-driven remains the default; realtime and
 * hybrid publish wall-clock-derived horizons consumed by Core 2. */
void core_chipset_timeline_init(uint64_t host_counter, uint64_t host_frequency,
                                RuntimeTimelineMode mode);
uint64_t core_chipset_timeline_update(uint64_t host_counter);
/* Cross-core lifecycle requests. The caller only publishes intent; Core 0
 * applies it on its next supervisor tick and remains the sole timeline owner. */
void core_chipset_timeline_request_pause(bool paused);
void core_chipset_timeline_request_mode(RuntimeTimelineMode mode);
RuntimeTimelineMode core_chipset_timeline_mode(void);
uint64_t core_chipset_get_horizon(void);
void core_chipset_get_timeline_snapshot(RuntimeTimeline *snapshot);
void core_chipset_drain_host_completions(void);
void core_chipset_get_frame_completion_stats(RuntimeFrameCompletionStats *stats);

#endif
