// src/cpu/emu68/bellatrix_profile.h
//
// MMIO profiling for the Emu68 bus path. Guarded by BELLATRIX_PROFILE so
// normal builds carry zero overhead.

#pragma once
#include <stdint.h>

#if defined(BELLATRIX_PROFILE) && BELLATRIX_PROFILE && \
    (defined(__aarch64__) || defined(_M_ARM64))
#define BELLATRIX_PROFILE_ENABLED 1
#else
#define BELLATRIX_PROFILE_ENABLED 0
#endif

#if BELLATRIX_PROFILE_ENABLED

/* -------------------------------------------------------------------------
 * Per-subsection timing bucket
 * ---------------------------------------------------------------------- */
typedef struct
{
    uint64_t calls;
    uint64_t cycles_total;
    uint64_t cycles_min;
    uint64_t cycles_max;
} BellatrixProfileBucket;

/* -------------------------------------------------------------------------
 * Advance-notification statistics (cpu progress → machine advance)
 * ---------------------------------------------------------------------- */
typedef struct
{
    uint64_t calls;
    uint64_t cpu_cycles_total;
    uint64_t cpu_cycles_min;
    uint64_t cpu_cycles_max;
} BellatrixAdvanceStats;

/* -------------------------------------------------------------------------
 * Multicore CPU->chipset publication statistics
 * ---------------------------------------------------------------------- */
typedef struct
{
    uint64_t publish_calls;
    uint64_t publish_m68k_cycles_total;
    uint64_t publish_cck_total;
    uint64_t wakeups;
    uint64_t chipset_steps;
    uint64_t chipset_cck_total;
    uint64_t target_cck_max;
    uint64_t chipset_cck_max;
    uint64_t backlog_cck_max;
    uint64_t backlog_samples;
    uint64_t backlog_cck_total;
    uint64_t empty_host_steps;
    uint64_t critical_mmio_reads;
    uint64_t critical_mmio_writes;
    uint64_t critical_mmio_samples;
    uint64_t critical_mmio_backlog_total;
    uint64_t critical_mmio_backlog_max;
    uint64_t critical_mmio_caught_up;
    uint64_t beam_vposr_fast;
    uint64_t beam_vposr_fallback;
    uint64_t beam_vhposr_fast;
    uint64_t beam_vhposr_fallback;
    uint64_t beam_snapshot_miss;
    uint64_t posted_writes;
    uint64_t posted_writes_applied;
    uint64_t posted_queue_full_fallbacks;
    uint64_t posted_queue_depth_max;
} BellatrixMulticoreStats;

/* -------------------------------------------------------------------------
 * Top-N MMIO hotspot table
 * ---------------------------------------------------------------------- */
#define BPROF_TOP_MMIO 16
#define BPROF_MMIO_SLOTS 256
typedef struct
{
    uint32_t addr;
    uint32_t last_pc;
    uint64_t count;
    uint64_t reads;
    uint64_t writes;
    uint8_t  used;
} BellatrixHotMMIO;

/* -------------------------------------------------------------------------
 * Aggregate profile state
 * ---------------------------------------------------------------------- */
typedef struct
{
    /* bellatrix_bus_access breakdown */
    BellatrixProfileBucket poll;          /* PAL_Runtime_Poll() cost         */
    BellatrixProfileBucket addr_fix;      /* slow_contains check + normalize */
    BellatrixProfileBucket lock_wait;     /* chipset_lock_acquire spin time  */
    BellatrixProfileBucket dispatch_read; /* bellatrix_bridge_cpu_read cost  */
    BellatrixProfileBucket dispatch_write;/* bellatrix_bridge_cpu_write cost */
    BellatrixProfileBucket total_access;  /* full bellatrix_bus_access cost  */

    /* Region breakdown (after addr_fix) */
    BellatrixProfileBucket region_cia_a;
    BellatrixProfileBucket region_cia_b;
    BellatrixProfileBucket region_ocs_intr;
    BellatrixProfileBucket region_ocs_other;
    BellatrixProfileBucket region_other;

    /* CPU progress / machine advance breakdown */
    BellatrixProfileBucket advance_time;
    BellatrixProfileBucket publish_time;
    BellatrixProfileBucket chipset_step_time;
    BellatrixAdvanceStats  advance_stats;
    BellatrixMulticoreStats multicore;

    /* Direct bridge dispatch reference path (no Emu68 page-fault overhead).
     * Compare with total_access to isolate fault cost. */
    BellatrixProfileBucket bridge_ref_read;
    BellatrixProfileBucket bridge_ref_write;

    /* Counters */
    uint64_t reads;
    uint64_t writes;
    uint64_t profile_start_counter;
    uint32_t profile_start_frame;
    uint32_t last_mmio_addr;
    uint32_t last_mmio_pc;

    /* Hotspot table */
    BellatrixHotMMIO hot[BPROF_MMIO_SLOTS];
} BellatrixProfile;

extern BellatrixProfile g_bprof;

/* -------------------------------------------------------------------------
 * Timer — CNTPCT_EL0 (bare-metal AArch64)
 * ---------------------------------------------------------------------- */
static inline uint64_t bprof_now(void)
{
    uint64_t t;
    asm volatile("mrs %0, CNTPCT_EL0" : "=r"(t));
    return t;
}

static inline uint64_t bprof_freq(void)
{
    uint64_t f;
    asm volatile("mrs %0, CNTFRQ_EL0" : "=r"(f));
    return f;
}

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */
void bprof_record(BellatrixProfileBucket *b, uint64_t elapsed);
void bprof_hot_record(uint32_t addr, uint32_t pc, int dir);
void bprof_multicore_publish(uint32_t m68k_cycles, uint32_t cck,
                             uint64_t target_cck);
void bprof_multicore_chipset_step(uint32_t cck_step,
                                  uint64_t chipset_cck,
                                  uint64_t target_cck);
void bprof_multicore_empty_host_step(uint64_t chipset_cck,
                                     uint64_t target_cck);
void bprof_multicore_critical_mmio(uint32_t addr, int is_write,
                                   uint64_t chipset_cck,
                                   uint64_t target_cck);
void bprof_multicore_beam_read(uint32_t addr, int projected,
                               int snapshot_available);
void bprof_multicore_posted_queued(uint32_t depth);
void bprof_multicore_posted_full_fallback(void);
void bprof_multicore_posted_applied(uint32_t count);
void bellatrix_profile_dump(void);
void bellatrix_profile_reset(void);

#define BPROF_CONTROL_ADDR 0xDFFF04u

/* Auto-dump every N total_access calls */
#define BPROF_AUTODUMP_INTERVAL 100000u

#endif /* BELLATRIX_PROFILE_ENABLED */
