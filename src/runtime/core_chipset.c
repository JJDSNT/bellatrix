// src/runtime/core_chipset.c
//
// Core 2 — Rigel chipset domain.
//
// Owns the full chipset tick (Agnus, Denise, Paula, CIA) via rigel_step().
// Runs on Core 2 in multicore mode; driven from the single-core poll path
// via bellatrix_runtime_host_step() when multicore is disabled.
//
// Synchronisation with Core 1 (CPU):
//   - Core 1 calls bellatrix_runtime_publish_cpu_cycles() after each JIT
//     block (or Musashi quantum), publishing M68K cycles converted to CCK
//     into s_cpu_cck_target.
//   - Core 2 wakes on SEV, drains cycles until caught up, then WFEs again.
//   - IPL changes and frame events are published atomically back to Core 1.
//
// core_chipset_lock_acquire/release() guard chipset state from concurrent
// CPU-side bus access (called from cpu_bridge.c, not from this core).

#include "runtime/core_chipset.h"
#include "runtime/cpu_progress.h"
#include "cpu/emu68/bellatrix_profile.h"

#include <stdatomic.h>
#include <string.h>

#include "rigel/rigel.h"
#include "rigel/rigel_irq.h"
#include "machine/machine.h"
#include "debug/core_log.h"
#include "host/pal.h"

/* Max CCK the CPU (Core 1) may run ahead of the chipset (Core 2) before it
 * blocks and lets Core 2 catch up. Without this bound the target diverges
 * without limit (observed: chipset >400M CCK behind), making the emulated
 * machine's sense of time meaningless. ~36 scanlines; tunable. First
 * correctness increment of the Core-0 arbiter (issue_core0_arbiter_scheduler.md). */
#define CHIPSET_MAX_BACKLOG_CCK 8192u

/* Minimum CCK per cross-core publication. Emu68 reports progress per JIT
 * block (~23 CCK per transaction in the ISSUE-0048 QEMU A/B); aggregating
 * was tried at 227 CCK and REGRESSED beam-poll workloads: with pending
 * cycles held back, every critical-MMIO rendezvous had to flush and then
 * actually wait for Core 2 (caught_up went 11k -> 783k), converting a
 * usually-free check into a cross-core round trip per VHPOSR poll. Keep 1
 * (publish immediately) until beam reads stop requiring a rendezvous. */
#define CHIPSET_PUBLISH_MIN_CCK 1u

/* Published by Core 1 (CPU); consumed by Core 2 (chipset). */
static _Atomic uint64_t s_cpu_cck_target = 0;

/* Aggregated CCK not yet added to s_cpu_cck_target. Core 1 local. */
static uint32_t s_pending_cck = 0;

/* Advanced by Core 2 (chipset); read cross-core by Core 1 (backpressure) and
 * Core 0 (supervisor), so it must be atomic. */
static _Atomic uint64_t s_chipset_cck = 0;
static _Atomic uint8_t s_pending_ipl = 0;

/* Remainder for M68K→CCK conversion (local to Core 1 call site). */
static uint32_t s_m68k_rem = 0;

static RuntimeCoreChipset *s_core = NULL;

static void core_chipset_publish_flush(void);

/* Guards chipset state from concurrent CPU-side bus access. Held by the CPU
 * core (cpu_bridge.c) while dispatching MMIO; not needed by this core. */
#if defined(BELLATRIX_ENABLE_MULTICORE)
static atomic_flag s_chipset_access_lock = ATOMIC_FLAG_INIT;
#endif

void core_chipset_lock_acquire(void)
{
#if defined(BELLATRIX_ENABLE_MULTICORE)
    while (atomic_flag_test_and_set_explicit(&s_chipset_access_lock, memory_order_acquire))
        asm volatile("wfe" ::: "memory");
#endif
}

void core_chipset_lock_release(void)
{
#if defined(BELLATRIX_ENABLE_MULTICORE)
    atomic_flag_clear_explicit(&s_chipset_access_lock, memory_order_release);
    asm volatile("dsb sy\n\t sev" ::: "memory");
#endif
}

bool core_chipset_get_progress(uint64_t *chipset_cck, uint64_t *target_cck)
{
#if defined(BELLATRIX_ENABLE_MULTICORE)
    RuntimeCoreChipset *core = s_core;

    if (!core || !core->running)
        return false;

    if (chipset_cck)
        *chipset_cck = atomic_load_explicit(&s_chipset_cck,
                                            memory_order_acquire);
    if (target_cck)
        *target_cck = atomic_load_explicit(&s_cpu_cck_target,
                                           memory_order_acquire);
    return true;
#else
    (void)chipset_cck;
    (void)target_cck;
    return false;
#endif
}

uint8_t core_chipset_get_pending_ipl(void)
{
    return atomic_load_explicit(&s_pending_ipl, memory_order_acquire);
}

void core_chipset_set_pending_ipl(uint8_t ipl)
{
    atomic_store_explicit(&s_pending_ipl, (uint8_t)(ipl & 7u),
                          memory_order_release);
}

/* MMIO-critical barrier. The CPU (Core 1) may run up to CHIPSET_MAX_BACKLOG_CCK
 * ahead of the chipset (Core 2); before a critical register access it must let
 * Core 2 catch up so it reads/writes fresh state. Mirrors the single-core
 * path's "flush partial cycles on bus access" (machine_rigel_step.c). Core 2
 * SEVs after each host_step, waking this WFE. */
void core_chipset_wait_caught_up(void)
{
#if defined(BELLATRIX_ENABLE_MULTICORE)
    RuntimeCoreChipset *core = s_core;
    if (!core || !core->running)
        return;

    /* The rendezvous only means "chipset reached the CPU's time" if the
     * target includes every cycle the CPU has run — flush the aggregation. */
    core_chipset_publish_flush();

    for (;;) {
        uint64_t target = atomic_load_explicit(&s_cpu_cck_target,
                                               memory_order_relaxed);
        uint64_t chip   = atomic_load_explicit(&s_chipset_cck,
                                               memory_order_acquire);
        if (chip >= target)
            return;
        PAL_Runtime_WakeupChipset();   /* nudge Core 2 to drain */
        asm volatile("wfe" ::: "memory");
    }
#endif
}

bool core_chipset_init(RuntimeCoreChipset *core,
                       RigelContext *rigel,
                       BellatrixMachine *machine)
{
    if (!core || !rigel || !machine)
        return false;

    memset(core, 0, sizeof(*core));
    core->rigel   = rigel;
    core->machine = machine;
    core->running = true;

    s_core = core;

    atomic_store_explicit(&s_cpu_cck_target, 0u, memory_order_release);
    atomic_store_explicit(&s_chipset_cck, 0u, memory_order_release);
    atomic_store_explicit(&s_pending_ipl, 0u, memory_order_release);
    s_m68k_rem    = 0;
    s_pending_cck = 0;

    CORE2_LOG("chipset init (Rigel)");
    return true;
}

void core_chipset_shutdown(RuntimeCoreChipset *core)
{
    if (!core) return;
    core->running = false;
    s_core = NULL;
    CORE2_LOG("chipset shutdown cck=%llu", (unsigned long long)core->local_cycles);
}

void core_chipset_reset(RuntimeCoreChipset *core)
{
    if (!core) return;
    core->local_cycles = 0;
    atomic_store_explicit(&s_cpu_cck_target, 0u, memory_order_release);
    atomic_store_explicit(&s_chipset_cck, 0u, memory_order_release);
    atomic_store_explicit(&s_pending_ipl, 0u, memory_order_release);
    s_m68k_rem    = 0;
    s_pending_cck = 0;
    CORE2_LOG("chipset reset");
}

/* Backpressure: block until Core 2 drains the backlog below the bound, so
 * the CPU (Core 1) cannot run unbounded ahead of the chipset (Core 2).
 * Core 2 SEVs after every drain iteration, waking this WFE. */
static void core_chipset_apply_backpressure(void)
{
#if defined(BELLATRIX_ENABLE_MULTICORE)
    for (;;) {
        uint64_t tgt  = atomic_load_explicit(&s_cpu_cck_target,
                                             memory_order_relaxed);
        uint64_t chip = atomic_load_explicit(&s_chipset_cck,
                                             memory_order_acquire);
        if ((tgt - chip) <= CHIPSET_MAX_BACKLOG_CCK)
            break;
        if (!s_core || !s_core->running)
            break;
        asm volatile("wfe" ::: "memory");
    }
#endif
}

/* Add `cck` to the shared target in bounded chunks. A single oversized
 * publication — JIT catch-up after a fault-free stretch, or an idle STOP
 * window — previously pushed the target up to 272x past the backlog bound
 * and froze both cores while Core 2 drained it blind (ISSUE-0048). */
static void core_chipset_publish_target(uint32_t cck)
{
    while (cck > 0u) {
        uint32_t chunk = cck > CHIPSET_MAX_BACKLOG_CCK
                       ? CHIPSET_MAX_BACKLOG_CCK
                       : cck;

        atomic_fetch_add_explicit(&s_cpu_cck_target, chunk,
                                  memory_order_release);
        PAL_Runtime_WakeupChipset();        /* sev — wake Core 2 */
        cck -= chunk;

        core_chipset_apply_backpressure();
    }
}

/* Flush the local aggregation into the shared target. Core 1 only. */
static void core_chipset_publish_flush(void)
{
    uint32_t publish = s_pending_cck;

    if (publish == 0u)
        return;
    s_pending_cck = 0u;

    XCORE_LOG("CPU->CHIPSET", "cck=%u target=%llu",
              (unsigned)publish,
              (unsigned long long)(atomic_load_explicit(&s_cpu_cck_target,
                                                        memory_order_relaxed)
                                   + publish));
    core_chipset_publish_target(publish);
}

/* ---------------------------------------------------------------------------
 * Called by Core 1 (CPU / JIT or Musashi) after each advance quantum.
 * Converts M68K cycles → CCK, aggregates locally, and signals Core 2 once
 * at least CHIPSET_PUBLISH_MIN_CCK accumulated.
 * ------------------------------------------------------------------------- */
void bellatrix_runtime_publish_cpu_cycles(uint32_t m68k_cycles)
{
#if BELLATRIX_PROFILE_ENABLED
    uint64_t t0 = bprof_now();
#endif
    uint32_t total = s_m68k_rem + m68k_cycles;
    uint32_t cck   = total >> 1u;          /* M68K / 2 = CCK */
    s_m68k_rem     = total & 1u;

    s_pending_cck += cck;
    if (s_pending_cck >= CHIPSET_PUBLISH_MIN_CCK)
        core_chipset_publish_flush();

#if BELLATRIX_PROFILE_ENABLED
    bprof_multicore_publish(m68k_cycles, cck,
                            atomic_load_explicit(&s_cpu_cck_target,
                                                 memory_order_acquire));
    bprof_record(&g_bprof.publish_time, bprof_now() - t0);
#endif
}

/* ---------------------------------------------------------------------------
 * Called by Core 2 from chipset_core_loop() in pal_core.c.
 * Advances Rigel until caught up with the CPU target.
 * ------------------------------------------------------------------------- */
void bellatrix_runtime_host_step(uint64_t now, uint64_t freq)
{
    (void)now;
    (void)freq;

    RuntimeCoreChipset *core = s_core;
    if (!core || !core->rigel || !core->running)
        return;

    uint64_t target = atomic_load_explicit(&s_cpu_cck_target, memory_order_acquire);
    uint64_t chip   = atomic_load_explicit(&s_chipset_cck, memory_order_relaxed);

    if (chip >= target) {
#if BELLATRIX_PROFILE_ENABLED
        bprof_multicore_empty_host_step(chip, target);
#endif
        bellatrix_machine_post_chipset_step();
        return;
    }

    while (chip < target) {
        uint64_t remaining = target - chip;
        rigel_cycle_t rigel_now = rigel_get_time(core->rigel);
        rigel_cycle_t until = rigel_now + (rigel_cycle_t)remaining;
        rigel_cycle_t next;

        /* Match the synchronous scheduler: stop at host-observable Rigel
         * events.  Internal DMA slots are processed by rigel_step_until().
         * Bellatrix does not currently stall the CPU from rigel_bus_state_t,
         * so cutting every temporal bus transition would add slot-by-slot
         * rendezvous without consuming its arbitration result.  The
         * CPU-published target remains the hard upper bound. */
        next = rigel_get_next_observable_deadline(core->rigel);
        if (next > rigel_now && next < until)
            until = next;

        /* Defensive fallback for a malformed/stale deadline.  remaining is
         * non-zero here, so this always makes forward progress. */
        if (until <= rigel_now)
            until = rigel_now + 1u;

#if BELLATRIX_PROFILE_ENABLED
        uint64_t t0 = bprof_now();
#endif
        /* The CPU bridge takes the same lock for every forwarded bus access.
         * Core 2 must participate too; otherwise the lock protects CPU callers
         * from each other but not from concurrent Rigel mutation here. */
        core_chipset_lock_acquire();
        rigel_step_result_t r = rigel_step_until(core->rigel, until);
        core_chipset_lock_release();
#if BELLATRIX_PROFILE_ENABLED
        bprof_record(&g_bprof.chipset_step_time, bprof_now() - t0);
#endif
        uint64_t advanced = (uint64_t)(r.time - rigel_now);
        chip               += advanced;
        core->local_cycles += advanced;
        core->machine->tick_count = (uint64_t)r.time;

        /* Publish progress every iteration, not only after the full drain:
         * Core 1's backpressure and critical-MMIO rendezvous otherwise wait
         * blind for the whole burst while a long backlog drains, freezing
         * the machine for seconds (ISSUE-0048). Store before event handling
         * so a frame present does not delay the CPU's wakeup. */
        atomic_store_explicit(&s_chipset_cck, chip, memory_order_release);
#if defined(BELLATRIX_ENABLE_MULTICORE)
        asm volatile("dsb ishst\n\tsev" ::: "memory");
#endif

        bellatrix_machine_on_chipset_advanced((uint32_t)advanced);
#if BELLATRIX_PROFILE_ENABLED
        bprof_multicore_chipset_step((uint32_t)advanced, chip, target);
#endif

        /* rigel_step_until() should reach `until`; avoid an infinite Core 2
         * loop if a future Rigel implementation returns without advancing. */
        if (advanced == 0u)
            break;

        if (r.events & RIGEL_EVENT_IRQ_CHANGED) {
            uint8_t ipl = (uint8_t)rigel_get_ipl(core->rigel);
            core_chipset_set_pending_ipl(ipl);
        }

        if (r.events & RIGEL_EVENT_FRAME_READY)
            bellatrix_machine_on_frame_ready();

        if (r.events & RIGEL_EVENT_HBLANK)
            bellatrix_machine_on_audio_sample_ready();
    }

    /* Publish progress and wake Core 1 if it is blocked on backpressure. */
    atomic_store_explicit(&s_chipset_cck, chip, memory_order_release);
#if defined(BELLATRIX_ENABLE_MULTICORE)
    asm volatile("dsb ishst\n\tsev" ::: "memory");
#endif

    bellatrix_machine_post_chipset_step();
}
