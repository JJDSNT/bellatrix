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
#include "runtime/posted_writes.h"
#include "cpu/emu68/bellatrix_profile.h"

#include <stdatomic.h>
#include <string.h>

#include "rigel/rigel.h"
#include "rigel/rigel_irq.h"
#include "rigel/rigel_mmio.h"
#include "rigel/rigel_custom.h"
#include "machine/machine.h"
#include "debug/core_log.h"
#include "host/pal.h"

/* Max CCK the CPU (Core 1) may run ahead of the chipset (Core 2) before it
 * blocks and lets Core 2 catch up. Without this bound the target diverges
 * without limit (observed: chipset >400M CCK behind), making the emulated
 * machine's sense of time meaningless. ~36 scanlines; tunable. First
 * correctness increment of the Core-0 arbiter (issue_core0_arbiter_scheduler.md). */
#define CHIPSET_MAX_BACKLOG_CCK 8192u
#define CHIPSET_MAX_DRAIN_BURST_CCK 8192u
#define CHIPSET_PAL_CCK_PER_SECOND 3546895u

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
static _Atomic uint64_t s_chipset_horizon = 0;
static _Atomic uint32_t s_timeline_mode = RUNTIME_TIMELINE_CPU_DRIVEN;
static _Atomic bool s_timeline_rebase_requested = false;
enum {
    TIMELINE_PAUSE_NONE = 0u,
    TIMELINE_PAUSE_REQUESTED = 1u,
    TIMELINE_RESUME_REQUESTED = 2u,
    TIMELINE_MODE_NONE = UINT32_MAX,
};
static _Atomic uint32_t s_timeline_pause_request = TIMELINE_PAUSE_NONE;
static _Atomic uint32_t s_timeline_mode_request = TIMELINE_MODE_NONE;
static RuntimeTimeline s_timeline; /* Core 0 owner */

/* Aggregated CCK not yet added to s_cpu_cck_target. Core 1 local. */
static uint32_t s_pending_cck = 0;

/* Advanced by Core 2 (chipset); read cross-core by Core 1 (backpressure) and
 * Core 0 (supervisor), so it must be atomic. */
static _Atomic uint64_t s_chipset_cck = 0;
static _Atomic uint8_t s_pending_ipl = 0;
static _Atomic uint32_t s_pending_frames = 0;

/* Hot read-only registers, published by the Rigel owner and served lock-free
 * to Core 1 poll loops (blit-busy, interrupt waits) without the critical-MMIO
 * rendezvous. Same pattern as s_pending_ipl; conceptually the PiStorm
 * housekeeper, which pushes hot bus state to the CPU instead of letting the
 * CPU fetch it. Refreshed on every Core 2 drain iteration and, for
 * read-own-write consistency, right after critical CPU writes (both under
 * the chipset access lock). VPOSR/VHPOSR stay on the slow path: they change
 * every CCK, so they are derived from a published beam geometry snapshot at
 * the CPU's logical time rather than copied as scalar register values. */
static _Atomic uint16_t s_pub_dmaconr = 0;
static _Atomic uint16_t s_pub_intenar = 0;
static _Atomic uint16_t s_pub_intreqr = 0;

/* Beam snapshot seqlock. Every payload field is atomic as C does not permit
 * concurrent non-atomic reads even when a sequence counter detects tearing.
 * The Rigel owner is the writer; Core 1 retries if publication overlaps. */
static _Atomic uint32_t s_pub_beam_seq = 0;
static _Atomic uint64_t s_pub_beam_time = 0;
static _Atomic uint16_t s_pub_beam_vpos = 0;
static _Atomic uint16_t s_pub_beam_hpos = 0;
static _Atomic uint16_t s_pub_beam_line_clocks = 0;
static _Atomic uint16_t s_pub_beam_frame_lines = 0;
static _Atomic uint16_t s_pub_beam_vposr_high = 0;
static _Atomic uint8_t s_pub_beam_lof = 0;
static _Atomic uint8_t s_pub_beam_lol = 0;
static _Atomic uint8_t s_pub_beam_lof_toggle = 0;
static _Atomic uint8_t s_pub_beam_lol_toggle = 0;

/* -----------------------------------------------------------------------
 * Posted-write queue (PiStorm wb_push/wb_task pattern, with timestamps).
 *
 * Non-critical custom-register writes from Core 1 are queued with the CPU's
 * emulated time and applied by Core 2 exactly when the Rigel reaches that
 * stamp — no lock, no rendezvous on the CPU side, and better temporal
 * fidelity than the old inline apply (which landed at the chipset's stale
 * time). Ordering rules that keep this safe:
 *   - reads and critical writes drain the queue under the access lock
 *     before acting, so program order is preserved where it is observable;
 *   - wait_caught_up (chip == target) already implies every posted stamp
 *     has been consumed, since stamps never exceed the published target.
 * Single producer (Core 1); consumers (Core 2 at stamps, Core 1 forced
 * drain) always hold the chipset access lock while applying.
 *
 * The ring itself lives in runtime/posted_writes.[ch] (pure SPSC, host
 * unit-tested); this file owns only the wait/fallback policy around it. */
static PostedWriteQueue s_pw_queue;

/* Remainder for M68K→CCK conversion (local to Core 1 call site). */
static uint32_t s_m68k_rem = 0;

static _Atomic(RuntimeCoreChipset *) s_core = NULL;

static void core_chipset_publish_flush(void);

void core_chipset_timeline_init(uint64_t host_counter,
                                uint64_t host_frequency,
                                RuntimeTimelineMode mode)
{
    uint64_t chip = atomic_load_explicit(&s_chipset_cck, memory_order_acquire);

    runtime_timeline_init(&s_timeline, host_frequency,
                          CHIPSET_PAL_CCK_PER_SECOND,
                          CHIPSET_MAX_BACKLOG_CCK, host_counter, chip);
    runtime_timeline_set_mode(&s_timeline, mode, host_counter, chip);
    atomic_store_explicit(&s_timeline_mode, (uint32_t)mode,
                          memory_order_release);
    atomic_store_explicit(&s_chipset_horizon, chip, memory_order_release);
    PAL_Runtime_WakeupChipset();
}

uint64_t core_chipset_timeline_update(uint64_t host_counter)
{
    uint64_t cpu = atomic_load_explicit(&s_cpu_cck_target,
                                        memory_order_acquire);
    uint32_t requested_mode = atomic_exchange_explicit(
        &s_timeline_mode_request, TIMELINE_MODE_NONE, memory_order_acq_rel);
    uint32_t pause_request = atomic_exchange_explicit(
        &s_timeline_pause_request, TIMELINE_PAUSE_NONE, memory_order_acq_rel);

    if (requested_mode <= RUNTIME_TIMELINE_HYBRID) {
        uint64_t chip = atomic_load_explicit(&s_chipset_cck,
                                             memory_order_acquire);
        runtime_timeline_set_mode(&s_timeline,
                                  (RuntimeTimelineMode)requested_mode,
                                  host_counter, chip);
        atomic_store_explicit(&s_timeline_mode, requested_mode,
                              memory_order_release);
    }
    if (pause_request != TIMELINE_PAUSE_NONE) {
        uint64_t chip = atomic_load_explicit(&s_chipset_cck,
                                             memory_order_acquire);
        runtime_timeline_set_paused(
            &s_timeline, pause_request == TIMELINE_PAUSE_REQUESTED,
            host_counter, chip);
    }
    if (atomic_exchange_explicit(&s_timeline_rebase_requested, false,
                                 memory_order_acq_rel)) {
        uint64_t chip = atomic_load_explicit(&s_chipset_cck,
                                             memory_order_acquire);
        runtime_timeline_set_mode(&s_timeline, core_chipset_timeline_mode(),
                                  host_counter, chip);
    }
    uint64_t horizon = runtime_timeline_update(&s_timeline, host_counter, cpu);

    atomic_store_explicit(&s_chipset_horizon, horizon, memory_order_release);
    return horizon;
}

void core_chipset_timeline_request_pause(bool paused)
{
    atomic_store_explicit(&s_timeline_pause_request,
                          paused ? TIMELINE_PAUSE_REQUESTED
                                 : TIMELINE_RESUME_REQUESTED,
                          memory_order_release);
    PAL_Runtime_WakeupChipset();
}

void core_chipset_timeline_request_mode(RuntimeTimelineMode mode)
{
    if (mode > RUNTIME_TIMELINE_HYBRID)
        return;
    atomic_store_explicit(&s_timeline_mode_request, (uint32_t)mode,
                          memory_order_release);
    PAL_Runtime_WakeupChipset();
}

RuntimeTimelineMode core_chipset_timeline_mode(void)
{
    return (RuntimeTimelineMode)atomic_load_explicit(&s_timeline_mode,
                                                     memory_order_acquire);
}

uint64_t core_chipset_get_horizon(void)
{
    return atomic_load_explicit(&s_chipset_horizon, memory_order_acquire);
}

void core_chipset_get_timeline_snapshot(RuntimeTimeline *snapshot)
{
    if (snapshot != NULL)
        *snapshot = s_timeline; /* Core 0 is the sole caller/owner. */
}

void core_chipset_drain_host_completions(void)
{
    uint32_t frames = atomic_exchange_explicit(&s_pending_frames, 0u,
                                                memory_order_acq_rel);

    while (frames-- != 0u)
        bellatrix_machine_on_frame_ready();
    bellatrix_machine_host_audio_poll();
}

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
    RuntimeCoreChipset *core = atomic_load_explicit(&s_core,
                                                    memory_order_acquire);

    if (!core || !atomic_load_explicit(&core->running, memory_order_acquire))
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

/* Caller must hold the chipset access lock (or be the only Rigel user). */
void core_chipset_publish_hot_regs(void)
{
    RuntimeCoreChipset *core = atomic_load_explicit(&s_core,
                                                    memory_order_acquire);
    rigel_beam_geometry_t beam;

    if (!core || !core->rigel)
        return;

    atomic_store_explicit(&s_pub_dmaconr,
                          rigel_custom_read16(core->rigel, 0x002u /* DMACONR */),
                          memory_order_release);
    atomic_store_explicit(&s_pub_intenar,
                          rigel_custom_read16(core->rigel, RIGEL_REG_INTENAR),
                          memory_order_release);
    atomic_store_explicit(&s_pub_intreqr,
                          rigel_custom_read16(core->rigel, RIGEL_REG_INTREQR),
                          memory_order_release);

    beam = rigel_get_beam_geometry(core->rigel);
    atomic_fetch_add_explicit(&s_pub_beam_seq, 1u, memory_order_acq_rel);
    atomic_store_explicit(&s_pub_beam_time, beam.time, memory_order_relaxed);
    atomic_store_explicit(&s_pub_beam_vpos, beam.vpos, memory_order_relaxed);
    atomic_store_explicit(&s_pub_beam_hpos, beam.hpos, memory_order_relaxed);
    atomic_store_explicit(&s_pub_beam_line_clocks, beam.line_clocks,
                          memory_order_relaxed);
    atomic_store_explicit(&s_pub_beam_frame_lines, beam.frame_lines,
                          memory_order_relaxed);
    atomic_store_explicit(&s_pub_beam_vposr_high, beam.vposr_high,
                          memory_order_relaxed);
    atomic_store_explicit(&s_pub_beam_lof, beam.lof, memory_order_relaxed);
    atomic_store_explicit(&s_pub_beam_lol, beam.lol, memory_order_relaxed);
    atomic_store_explicit(&s_pub_beam_lof_toggle, beam.lof_toggle,
                          memory_order_relaxed);
    atomic_store_explicit(&s_pub_beam_lol_toggle, beam.lol_toggle,
                          memory_order_relaxed);
    atomic_fetch_add_explicit(&s_pub_beam_seq, 1u, memory_order_release);
}

#if defined(BELLATRIX_ENABLE_MULTICORE)
/* Consumed only by the multicore hot-read fast path. */
static bool core_chipset_read_beam_geometry(rigel_beam_geometry_t *beam)
{
    uint32_t before;
    uint32_t after;

    if (!beam)
        return false;

    for (;;) {
        before = atomic_load_explicit(&s_pub_beam_seq, memory_order_acquire);
        if (before & 1u)
            continue;
        beam->time = atomic_load_explicit(&s_pub_beam_time, memory_order_relaxed);
        beam->vpos = atomic_load_explicit(&s_pub_beam_vpos, memory_order_relaxed);
        beam->hpos = atomic_load_explicit(&s_pub_beam_hpos, memory_order_relaxed);
        beam->line_clocks = atomic_load_explicit(&s_pub_beam_line_clocks,
                                                 memory_order_relaxed);
        beam->frame_lines = atomic_load_explicit(&s_pub_beam_frame_lines,
                                                 memory_order_relaxed);
        beam->vposr_high = atomic_load_explicit(&s_pub_beam_vposr_high,
                                                memory_order_relaxed);
        beam->lof = atomic_load_explicit(&s_pub_beam_lof, memory_order_relaxed);
        beam->lol = atomic_load_explicit(&s_pub_beam_lol, memory_order_relaxed);
        beam->lof_toggle = atomic_load_explicit(&s_pub_beam_lof_toggle,
                                                memory_order_relaxed);
        beam->lol_toggle = atomic_load_explicit(&s_pub_beam_lol_toggle,
                                                memory_order_relaxed);
        after = atomic_load_explicit(&s_pub_beam_seq, memory_order_acquire);
        if (before == after)
            break;
    }

    return before != 0u;
}
#endif /* BELLATRIX_ENABLE_MULTICORE */

/* Post one non-critical custom write. CPU-driven mode preserves its logical
 * timestamp; self-paced modes apply at chipset "now", matching hardware.
 * Returns false when unavailable so the caller uses the sync path. */
bool core_chipset_post_write(uint32_t addr, uint32_t value, uint32_t size)
{
#if defined(BELLATRIX_ENABLE_MULTICORE)
    RuntimeCoreChipset *core = atomic_load_explicit(&s_core,
                                                    memory_order_acquire);
    uint64_t stamp;
    PostedWrite write;
    bool waited_for_space = false;

    if (!core || !atomic_load_explicit(&core->running, memory_order_acquire))
        return false;

    /* The CPU's emulated "now": everything it published plus the local
     * aggregation. Never ahead of what Core 2 will eventually reach. */
    if (core_chipset_timeline_mode() == RUNTIME_TIMELINE_CPU_DRIVEN) {
        stamp = atomic_load_explicit(&s_cpu_cck_target, memory_order_relaxed) +
                s_pending_cck;
    } else {
        stamp = atomic_load_explicit(&s_chipset_cck, memory_order_acquire);
    }

    write = (PostedWrite){
        .stamp_cck = stamp, .addr = addr, .value = value, .size = size,
    };

    /* Full: wait for consumers. Core 2 drains on every host step — including
     * the caught-up and paused paths — and SEVs after each drain iteration.
     * The wait policy lives here, not in the queue, so shutdown can always
     * bail out to the caller's synchronous fallback. */
    while (!posted_writes_try_push(&s_pw_queue, &write)) {
        waited_for_space = true;
        if (!atomic_load_explicit(&core->running, memory_order_acquire))
            return false;
        PAL_Runtime_WakeupChipset();
        asm volatile("wfe" ::: "memory");
    }

#if BELLATRIX_PROFILE_ENABLED
    bprof_multicore_posted_queued(posted_writes_depth(&s_pw_queue),
                                  waited_for_space);
#else
    (void)waited_for_space;
#endif
    PAL_Runtime_WakeupChipset();
    return true;
#else
    (void)addr; (void)value; (void)size;
    return false;
#endif
}

static void core_chipset_apply_one_posted(void *ctx, const PostedWrite *w)
{
    (void)ctx;
    bellatrix_machine_write(w->addr, w->value, w->size);
}

/* Apply queued writes with stamp <= limit (UINT64_MAX = drain everything).
 * Caller must hold the chipset access lock. */
static void core_chipset_apply_posted_writes(uint64_t limit)
{
    uint32_t applied = posted_writes_apply(&s_pw_queue, limit,
                                           core_chipset_apply_one_posted,
                                           NULL);

#if BELLATRIX_PROFILE_ENABLED
    if (applied != 0u)
        bprof_multicore_posted_applied(applied);
#else
    (void)applied;
#endif
}

/* Forced drain before a CPU-side read or critical write (lock held). */
void core_chipset_drain_posted_writes(void)
{
    core_chipset_apply_posted_writes(UINT64_MAX);
}

/* Earliest pending stamp, or UINT64_MAX when the queue is empty. Core 2. */
static uint64_t core_chipset_next_posted_stamp(void)
{
    return posted_writes_next_stamp(&s_pw_queue);
}

/* Lock-free fast path for hot-register reads on Core 1. Returns false when
 * the register is not published or multicore is not active, sending the
 * caller down the regular rendezvous+lock path. */
bool core_chipset_read_hot_reg(uint32_t normalized_addr, uint32_t *value)
{
#if defined(BELLATRIX_ENABLE_MULTICORE)
    RuntimeCoreChipset *core = atomic_load_explicit(&s_core,
                                                    memory_order_acquire);
    rigel_beam_geometry_t beam;
    rigel_cycle_t cpu_time;
    rigel_u16 vpos;
    rigel_u16 hpos;

    if (!core ||
        !atomic_load_explicit(&core->running, memory_order_acquire) || !value)
        return false;

    switch (normalized_addr) {
    case 0x00DFF002u:   /* DMACONR */
        *value = atomic_load_explicit(&s_pub_dmaconr, memory_order_acquire);
        return true;
    case 0x00DFF01Cu:   /* INTENAR */
        *value = atomic_load_explicit(&s_pub_intenar, memory_order_acquire);
        return true;
    case 0x00DFF01Eu:   /* INTREQR */
        *value = atomic_load_explicit(&s_pub_intreqr, memory_order_acquire);
        return true;
    case 0x00DFF004u:   /* VPOSR */
    case 0x00DFF006u:   /* VHPOSR */
        if (!core_chipset_read_beam_geometry(&beam)) {
#if BELLATRIX_PROFILE_ENABLED
            bprof_multicore_beam_read(normalized_addr, 0, 0);
#endif
            return false;
        }
        if (core_chipset_timeline_mode() == RUNTIME_TIMELINE_CPU_DRIVEN) {
            cpu_time = atomic_load_explicit(&s_cpu_cck_target,
                                            memory_order_acquire) +
                       s_pending_cck;
        } else {
            cpu_time = atomic_load_explicit(&s_chipset_cck,
                                            memory_order_acquire);
        }
        if (!rigel_beam_position_at(&beam, cpu_time, &vpos, &hpos)) {
#if BELLATRIX_PROFILE_ENABLED
            bprof_multicore_beam_read(normalized_addr, 0, 1);
#endif
            return false;
        }
        if (normalized_addr == 0x00DFF004u)
            *value = (uint32_t)(beam.vposr_high | ((vpos >> 8) & 0x7u));
        else
            *value = (uint32_t)(((vpos & 0xffu) << 8) |
                                ((hpos >> 1) & 0xffu));
#if BELLATRIX_PROFILE_ENABLED
        bprof_multicore_beam_read(normalized_addr, 1, 1);
#endif
        return true;
    default:
        return false;
    }
#else
    (void)normalized_addr;
    (void)value;
    return false;
#endif
}

/* MMIO-critical barrier. The CPU (Core 1) may run up to CHIPSET_MAX_BACKLOG_CCK
 * ahead of the chipset (Core 2); before a critical register access it must let
 * Core 2 catch up so it reads/writes fresh state. Mirrors the single-core
 * path's "flush partial cycles on bus access" (machine_rigel_step.c). Core 2
 * SEVs after each host_step, waking this WFE. */
void core_chipset_wait_caught_up(void)
{
#if defined(BELLATRIX_ENABLE_MULTICORE)
    RuntimeCoreChipset *core = atomic_load_explicit(&s_core,
                                                    memory_order_acquire);
    if (!core || !atomic_load_explicit(&core->running, memory_order_acquire))
        return;
    if (core_chipset_timeline_mode() != RUNTIME_TIMELINE_CPU_DRIVEN)
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
    atomic_store_explicit(&core->running, true, memory_order_release);

    atomic_store_explicit(&s_core, core, memory_order_release);

    atomic_store_explicit(&s_cpu_cck_target, 0u, memory_order_release);
    atomic_store_explicit(&s_chipset_horizon, 0u, memory_order_release);
    atomic_store_explicit(&s_timeline_mode, RUNTIME_TIMELINE_CPU_DRIVEN,
                          memory_order_release);
    atomic_store_explicit(&s_timeline_rebase_requested, false,
                          memory_order_release);
    atomic_store_explicit(&s_timeline_pause_request, TIMELINE_PAUSE_NONE,
                          memory_order_release);
    atomic_store_explicit(&s_timeline_mode_request, TIMELINE_MODE_NONE,
                          memory_order_release);
    atomic_store_explicit(&s_chipset_cck, 0u, memory_order_release);
    atomic_store_explicit(&s_pending_ipl, 0u, memory_order_release);
    atomic_store_explicit(&s_pending_frames, 0u, memory_order_release);
    s_m68k_rem    = 0;
    s_pending_cck = 0;
    posted_writes_reset(&s_pw_queue);
    atomic_store_explicit(&s_pub_beam_seq, 0u, memory_order_release);
    core_chipset_publish_hot_regs();

    CORE2_LOG("chipset init (Rigel)");
    return true;
}

void core_chipset_shutdown(RuntimeCoreChipset *core)
{
    if (!core) return;
    atomic_store_explicit(&core->running, false, memory_order_release);
    atomic_store_explicit(&s_pub_beam_seq, 0u, memory_order_release);
    PAL_Runtime_WakeupChipset(); /* release queue-full/backpressure waiters */
    atomic_store_explicit(&s_core, NULL, memory_order_release);
    CORE2_LOG("chipset shutdown cck=%llu", (unsigned long long)core->local_cycles);
}

void core_chipset_reset(RuntimeCoreChipset *core)
{
    if (!core) return;
    core->local_cycles = 0;
    atomic_store_explicit(&s_cpu_cck_target, 0u, memory_order_release);
    atomic_store_explicit(&s_chipset_horizon, 0u, memory_order_release);
    atomic_store_explicit(&s_timeline_rebase_requested, true,
                          memory_order_release);
    atomic_store_explicit(&s_timeline_pause_request, TIMELINE_PAUSE_NONE,
                          memory_order_release);
    atomic_store_explicit(&s_timeline_mode_request, TIMELINE_MODE_NONE,
                          memory_order_release);
    atomic_store_explicit(&s_chipset_cck, 0u, memory_order_release);
    atomic_store_explicit(&s_pending_ipl, 0u, memory_order_release);
    atomic_store_explicit(&s_pending_frames, 0u, memory_order_release);
    s_m68k_rem    = 0;
    s_pending_cck = 0;
    posted_writes_reset(&s_pw_queue);
    atomic_store_explicit(&s_pub_beam_seq, 0u, memory_order_release);
    core_chipset_publish_hot_regs();
    CORE2_LOG("chipset reset");
}

/* Backpressure: block until Core 2 drains the backlog below the bound, so
 * the CPU (Core 1) cannot run unbounded ahead of the chipset (Core 2).
 * Core 2 SEVs after every drain iteration, waking this WFE. */
static void core_chipset_apply_backpressure(void)
{
#if defined(BELLATRIX_ENABLE_MULTICORE)
    if (core_chipset_timeline_mode() != RUNTIME_TIMELINE_CPU_DRIVEN)
        return;
    for (;;) {
        uint64_t tgt  = atomic_load_explicit(&s_cpu_cck_target,
                                             memory_order_relaxed);
        uint64_t chip = atomic_load_explicit(&s_chipset_cck,
                                             memory_order_acquire);
        if (tgt <= chip || (tgt - chip) <= CHIPSET_MAX_BACKLOG_CCK)
            break;
        RuntimeCoreChipset *core = atomic_load_explicit(&s_core,
                                                        memory_order_acquire);
        if (!core ||
            !atomic_load_explicit(&core->running, memory_order_acquire))
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
        if (core_chipset_timeline_mode() == RUNTIME_TIMELINE_CPU_DRIVEN) {
            atomic_store_explicit(
                &s_chipset_horizon,
                atomic_load_explicit(&s_cpu_cck_target, memory_order_acquire),
                memory_order_release);
        }
        /* The Core 2 timer event stream samples ordinary progress at a fixed
         * cadence. Contacts and backpressure retain explicit SEV wakeups. */
        if (!PAL_Runtime_EventStreamActive())
            PAL_Runtime_WakeupChipset();
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

    RuntimeCoreChipset *core = atomic_load_explicit(&s_core,
                                                    memory_order_acquire);
    if (!core || !core->rigel ||
        !atomic_load_explicit(&core->running, memory_order_acquire))
        return;

    uint64_t target = atomic_load_explicit(&s_chipset_horizon,
                                           memory_order_acquire);
    uint64_t chip   = atomic_load_explicit(&s_chipset_cck, memory_order_relaxed);

    /* A horizon is permission, not an obligation to monopolize Core 2 until
     * fully caught up. Bound one host-step burst, then return to WFE/contact
     * handling; the event stream will schedule the next slice. */
    if (target > chip && target - chip > CHIPSET_MAX_DRAIN_BURST_CCK)
        target = chip + CHIPSET_MAX_DRAIN_BURST_CCK;

    if (chip >= target) {
        /* Caught up, but stamps <= target may still sit in the queue (the
         * CPU posts and only then publishes the surrounding cycles). */
        if (core_chipset_next_posted_stamp() != UINT64_MAX) {
            core_chipset_lock_acquire();
            core_chipset_apply_posted_writes(UINT64_MAX);
            core_chipset_publish_hot_regs();
            core_chipset_lock_release();
        }
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

        /* Posted writes are applied at their exact stamp: never step past
         * the earliest pending one. */
        {
            uint64_t wstamp = core_chipset_next_posted_stamp();
            if (wstamp > rigel_now && wstamp < until)
                until = (rigel_cycle_t)wstamp;
        }

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
        core_chipset_apply_posted_writes((uint64_t)rigel_now);
        rigel_step_result_t r = rigel_step_until(core->rigel, until);
        core_chipset_apply_posted_writes((uint64_t)r.time);
        core_chipset_publish_hot_regs();
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
            atomic_fetch_add_explicit(&s_pending_frames, 1u,
                                      memory_order_release);

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
