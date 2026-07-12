// src/cpu/emu68/bellatrix_profile.c

#include "cpu/emu68/bellatrix_profile.h"

#if BELLATRIX_PROFILE_ENABLED

#include <string.h>
#include "machine/machine.h"
#include "host/pal.h"
#include "cpu/emu68/bellatrix.h"
#include "support.h"  /* kprintf */

BellatrixProfile g_bprof;

static const char *bprof_cpu_backend_name(void)
{
#if defined(BELLATRIX_USE_MUSASHI_CPU) && BELLATRIX_USE_MUSASHI_CPU
    return "musashi";
#else
    return "emu68";
#endif
}

/* -------------------------------------------------------------------------
 * Bucket record
 * ---------------------------------------------------------------------- */
void bprof_record(BellatrixProfileBucket *b, uint64_t elapsed)
{
    b->calls++;
    b->cycles_total += elapsed;
    if (elapsed < b->cycles_min) b->cycles_min = elapsed;
    if (elapsed > b->cycles_max) b->cycles_max = elapsed;
}

static uint32_t bprof_addr_hash(uint32_t addr)
{
    addr ^= addr >> 12;
    addr ^= addr >> 6;
    return addr & (BPROF_MMIO_SLOTS - 1u);
}

/* -------------------------------------------------------------------------
 * Hotspot table — fixed-size exact hash table for the common case.  The old
 * top-N-only table lost repeated addresses once the first 16 slots filled.
 * ---------------------------------------------------------------------- */
void bprof_hot_record(uint32_t addr, uint32_t pc, int dir)
{
    uint32_t slot = bprof_addr_hash(addr);

    g_bprof.last_mmio_addr = addr;
    g_bprof.last_mmio_pc = pc;

    for (uint32_t probe = 0; probe < BPROF_MMIO_SLOTS; probe++) {
        BellatrixHotMMIO *h = &g_bprof.hot[(slot + probe) & (BPROF_MMIO_SLOTS - 1u)];

        if (!h->used) {
            h->used = 1u;
            h->addr = addr;
            h->last_pc = pc;
            h->count = 0u;
            h->reads = 0u;
            h->writes = 0u;
        } else if (h->addr != addr) {
            continue;
        }

        h->count++;
        h->last_pc = pc;
        if (dir == BUS_WRITE)
            h->writes++;
        else
            h->reads++;
        return;
    }

    {
        uint32_t best = 0u;
        for (uint32_t i = 1; i < BPROF_MMIO_SLOTS; i++) {
            if (g_bprof.hot[i].count < g_bprof.hot[best].count)
                best = i;
        }

        g_bprof.hot[best].used = 1u;
        g_bprof.hot[best].addr = addr;
        g_bprof.hot[best].last_pc = pc;
        g_bprof.hot[best].count = 1u;
        g_bprof.hot[best].reads = (dir == BUS_WRITE) ? 0u : 1u;
        g_bprof.hot[best].writes = (dir == BUS_WRITE) ? 1u : 0u;
    }
}

static void bprof_multicore_record_backlog(uint64_t chipset_cck,
                                           uint64_t target_cck)
{
    uint64_t backlog = target_cck > chipset_cck ? target_cck - chipset_cck : 0u;

    g_bprof.multicore.backlog_samples++;
    g_bprof.multicore.backlog_cck_total += backlog;
    if (backlog > g_bprof.multicore.backlog_cck_max)
        g_bprof.multicore.backlog_cck_max = backlog;
    if (target_cck > g_bprof.multicore.target_cck_max)
        g_bprof.multicore.target_cck_max = target_cck;
    if (chipset_cck > g_bprof.multicore.chipset_cck_max)
        g_bprof.multicore.chipset_cck_max = chipset_cck;
}

void bprof_multicore_publish(uint32_t m68k_cycles, uint32_t cck,
                             uint64_t target_cck)
{
    g_bprof.multicore.publish_calls++;
    g_bprof.multicore.publish_m68k_cycles_total += m68k_cycles;
    g_bprof.multicore.publish_cck_total += cck;
    if (cck > 0u && !PAL_Runtime_EventStreamActive())
        g_bprof.multicore.wakeups++;
    if (target_cck > g_bprof.multicore.target_cck_max)
        g_bprof.multicore.target_cck_max = target_cck;
}

void bprof_multicore_chipset_step(uint32_t cck_step,
                                  uint64_t chipset_cck,
                                  uint64_t target_cck)
{
    g_bprof.multicore.chipset_steps++;
    g_bprof.multicore.chipset_cck_total += cck_step;
    bprof_multicore_record_backlog(chipset_cck, target_cck);
}

void bprof_multicore_empty_host_step(uint64_t chipset_cck,
                                     uint64_t target_cck)
{
    g_bprof.multicore.empty_host_steps++;
    bprof_multicore_record_backlog(chipset_cck, target_cck);
}

void bprof_multicore_critical_mmio(uint32_t addr, int is_write,
                                   uint64_t chipset_cck,
                                   uint64_t target_cck)
{
    uint64_t backlog = target_cck > chipset_cck ? target_cck - chipset_cck : 0u;

    (void)addr;

    if (is_write)
        g_bprof.multicore.critical_mmio_writes++;
    else
        g_bprof.multicore.critical_mmio_reads++;

    g_bprof.multicore.critical_mmio_samples++;
    g_bprof.multicore.critical_mmio_backlog_total += backlog;
    if (backlog > g_bprof.multicore.critical_mmio_backlog_max)
        g_bprof.multicore.critical_mmio_backlog_max = backlog;
    if (backlog == 0u)
        g_bprof.multicore.critical_mmio_caught_up++;
}

void bprof_multicore_beam_read(uint32_t addr, int projected,
                               int snapshot_available)
{
    uint32_t reg = addr & 0x1ffu;

    if (reg == 0x004u) {
        if (projected)
            g_bprof.multicore.beam_vposr_fast++;
        else
            g_bprof.multicore.beam_vposr_fallback++;
    } else if (reg == 0x006u) {
        if (projected)
            g_bprof.multicore.beam_vhposr_fast++;
        else
            g_bprof.multicore.beam_vhposr_fallback++;
    }
    if (!snapshot_available)
        g_bprof.multicore.beam_snapshot_miss++;
}

void bprof_multicore_posted_queued(uint32_t depth)
{
    uint64_t old_max;

    __atomic_fetch_add(&g_bprof.multicore.posted_writes, 1u, __ATOMIC_RELAXED);
    old_max = __atomic_load_n(&g_bprof.multicore.posted_queue_depth_max,
                              __ATOMIC_RELAXED);
    while (old_max < depth &&
           !__atomic_compare_exchange_n(
               &g_bprof.multicore.posted_queue_depth_max, &old_max, depth,
               1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

void bprof_multicore_posted_full_fallback(void)
{
    __atomic_fetch_add(&g_bprof.multicore.posted_queue_full_fallbacks, 1u,
                       __ATOMIC_RELAXED);
}

void bprof_multicore_posted_applied(uint32_t count)
{
    __atomic_fetch_add(&g_bprof.multicore.posted_writes_applied, count,
                       __ATOMIC_RELAXED);
}

/* -------------------------------------------------------------------------
 * Reset — zero everything, re-init mins to UINT64_MAX
 * ---------------------------------------------------------------------- */
void bellatrix_profile_reset(void)
{
    const BellatrixMachine *m;

    memset(&g_bprof, 0, sizeof(g_bprof));

#define INIT_MIN(b) g_bprof.b.cycles_min = UINT64_MAX
    INIT_MIN(poll);
    INIT_MIN(addr_fix);
    INIT_MIN(lock_wait);
    INIT_MIN(dispatch_read);
    INIT_MIN(dispatch_write);
    INIT_MIN(total_access);
    INIT_MIN(region_cia_a);
    INIT_MIN(region_cia_b);
    INIT_MIN(region_ocs_intr);
    INIT_MIN(region_ocs_other);
    INIT_MIN(region_other);
    INIT_MIN(advance_time);
    INIT_MIN(publish_time);
    INIT_MIN(chipset_step_time);
    INIT_MIN(bridge_ref_read);
    INIT_MIN(bridge_ref_write);
#undef INIT_MIN
    g_bprof.advance_stats.cpu_cycles_min = UINT64_MAX;
    g_bprof.profile_start_counter = bprof_now();
    m = bellatrix_machine_get();
    g_bprof.profile_start_frame = m ? m->frame_counter : 0u;
}

/* -------------------------------------------------------------------------
 * Helpers for dump
 * ---------------------------------------------------------------------- */
static uint64_t avg_cy(const BellatrixProfileBucket *b)
{
    return b->calls ? b->cycles_total / b->calls : 0;
}

static uint64_t avg_ns(const BellatrixProfileBucket *b, uint64_t freq)
{
    if (!b->calls || !freq) return 0;
    return (b->cycles_total * 1000000000ull) / (b->calls * freq);
}

static uint64_t pct(const BellatrixProfileBucket *b,
                    const BellatrixProfileBucket *total)
{
    if (!total->cycles_total) return 0;
    return (b->cycles_total * 100ull) / total->cycles_total;
}

/* -------------------------------------------------------------------------
 * Dump
 * ---------------------------------------------------------------------- */
void bellatrix_profile_dump(void)
{
    const uint64_t freq = bprof_freq();
    const BellatrixMachine *m = bellatrix_machine_get();
    BellatrixProfile *p = &g_bprof;

    kprintf("[BPROF] ===== MMIO Profiling Report =====\n");
    kprintf("[BPROF] freq=%llu Hz\n", (unsigned long long)freq);
    kprintf("[BPROF] mode=%s  cpu_backend=%s\n",
            PAL_Core_IsMulticoreEnabled() ? "multicore" : "singlecore",
            bprof_cpu_backend_name());
    kprintf("[BPROF] pc: fault=%08X exec=%08X last_mmio=%06X last_mmio_pc=%08X\n",
            (unsigned)g_bellatrix_fault_pc,
            (unsigned)g_bellatrix_exec_pc,
            (unsigned)g_bprof.last_mmio_addr,
            (unsigned)g_bprof.last_mmio_pc);
    if (m) {
        uint64_t total_m68k = p->advance_stats.cpu_cycles_total
                            + p->multicore.publish_m68k_cycles_total;
        uint64_t now = bprof_now();
        uint64_t elapsed = p->profile_start_counter
                         ? now - p->profile_start_counter
                         : 0u;
        uint32_t frames = m->frame_counter - p->profile_start_frame;
        uint64_t elapsed_ms = (elapsed && freq)
                            ? (elapsed * 1000ull) / freq
                            : 0u;
        uint64_t fps_x100 = (elapsed && freq)
                          ? ((uint64_t)frames * freq * 100ull) / elapsed
                          : 0u;

        if (!p->profile_start_counter) {
            p->profile_start_counter = now;
            p->profile_start_frame = m->frame_counter;
        }

        kprintf("[BPROF] frame=%u (+%u)  elapsed=%llums  fps=%llu.%02llu  machine_tick_cck=%llu\n",
                (unsigned)m->frame_counter,
                (unsigned)frames,
                (unsigned long long)elapsed_ms,
                (unsigned long long)(fps_x100 / 100ull),
                (unsigned long long)(fps_x100 % 100ull),
                (unsigned long long)m->tick_count);
        kprintf("[BPROF] cycles: m68k=%llu  publish_cck=%llu  chipset_cck=%llu\n",
                (unsigned long long)total_m68k,
                (unsigned long long)p->multicore.publish_cck_total,
                (unsigned long long)p->multicore.chipset_cck_total);
        if (PAL_Core_IsMulticoreEnabled()) {
            uint64_t effective_backlog =
                p->multicore.target_cck_max > m->tick_count
                    ? p->multicore.target_cck_max - m->tick_count
                    : 0u;
            kprintf("[BPROF] multicore_effective_backlog_cck=%llu\n",
                    (unsigned long long)effective_backlog);
        }
    }
    kprintf("[BPROF] reads=%llu  writes=%llu\n",
            (unsigned long long)p->reads,
            (unsigned long long)p->writes);

    kprintf("[BPROF] --- bellatrix_bus_access breakdown ---\n");
    kprintf("[BPROF] total_access : calls=%llu  avg=%llu cy (%llu ns)  min=%llu  max=%llu\n",
            (unsigned long long)p->total_access.calls,
            (unsigned long long)avg_cy(&p->total_access),
            (unsigned long long)avg_ns(&p->total_access, freq),
            (unsigned long long)(p->total_access.calls ? p->total_access.cycles_min : 0),
            (unsigned long long)p->total_access.cycles_max);

#define DUMP_BUCKET(name, field) \
    kprintf("[BPROF]   %-12s : calls=%llu  avg=%llu cy (%llu ns)  %llu%%\n", \
            name, \
            (unsigned long long)p->field.calls, \
            (unsigned long long)avg_cy(&p->field), \
            (unsigned long long)avg_ns(&p->field, freq), \
            (unsigned long long)pct(&p->field, &p->total_access))

    DUMP_BUCKET("poll",       poll);
    DUMP_BUCKET("addr_fix",   addr_fix);
    DUMP_BUCKET("lock_wait",  lock_wait);
    DUMP_BUCKET("disp_read",  dispatch_read);
    DUMP_BUCKET("disp_write", dispatch_write);
#undef DUMP_BUCKET

    /* Derived: fault overhead = total - sum of measured sub-sections */
    {
        uint64_t measured = p->poll.cycles_total
                          + p->addr_fix.cycles_total
                          + p->lock_wait.cycles_total
                          + p->dispatch_read.cycles_total
                          + p->dispatch_write.cycles_total;
        uint64_t overhead = p->total_access.cycles_total > measured
                          ? p->total_access.cycles_total - measured : 0;
        uint64_t ovh_pct  = p->total_access.cycles_total
                          ? (overhead * 100ull) / p->total_access.cycles_total : 0;
        kprintf("[BPROF]   %-12s : (derived) total=%llu cy  %llu%%\n",
                "fault_ovhd",
                (unsigned long long)overhead,
                (unsigned long long)ovh_pct);
    }

    kprintf("[BPROF] --- CPU advance ---\n");
    kprintf("[BPROF] advance_time : calls=%llu  avg=%llu cy (%llu ns)\n",
            (unsigned long long)p->advance_time.calls,
            (unsigned long long)avg_cy(&p->advance_time),
            (unsigned long long)avg_ns(&p->advance_time, freq));
    {
        uint64_t avg_m68k = p->advance_stats.calls
                          ? p->advance_stats.cpu_cycles_total / p->advance_stats.calls
                          : 0;
        kprintf("[BPROF]   avg_m68k_cycles_per_call=%llu  min=%llu  max=%llu\n",
                (unsigned long long)avg_m68k,
                (unsigned long long)(p->advance_stats.calls ? p->advance_stats.cpu_cycles_min : 0),
                (unsigned long long)p->advance_stats.cpu_cycles_max);
    }

    kprintf("[BPROF] --- Multicore CPU->Chipset ---\n");
    kprintf("[BPROF] publish_time : calls=%llu  avg=%llu cy (%llu ns)\n",
            (unsigned long long)p->publish_time.calls,
            (unsigned long long)avg_cy(&p->publish_time),
            (unsigned long long)avg_ns(&p->publish_time, freq));
    kprintf("[BPROF] chipset_step : calls=%llu  avg=%llu cy (%llu ns)\n",
            (unsigned long long)p->chipset_step_time.calls,
            (unsigned long long)avg_cy(&p->chipset_step_time),
            (unsigned long long)avg_ns(&p->chipset_step_time, freq));
    {
        uint64_t avg_pub_m68k = p->multicore.publish_calls
                              ? p->multicore.publish_m68k_cycles_total /
                                p->multicore.publish_calls
                              : 0;
        uint64_t avg_pub_cck = p->multicore.publish_calls
                             ? p->multicore.publish_cck_total /
                               p->multicore.publish_calls
                             : 0;
        uint64_t avg_step_cck = p->multicore.chipset_steps
                              ? p->multicore.chipset_cck_total /
                                p->multicore.chipset_steps
                              : 0;
        uint64_t avg_backlog = p->multicore.backlog_samples
                             ? p->multicore.backlog_cck_total /
                               p->multicore.backlog_samples
                             : 0;
        kprintf("[BPROF]   publish_calls=%llu wakeups=%llu avg_m68k=%llu avg_cck=%llu\n",
                (unsigned long long)p->multicore.publish_calls,
                (unsigned long long)p->multicore.wakeups,
                (unsigned long long)avg_pub_m68k,
                (unsigned long long)avg_pub_cck);
        kprintf("[BPROF]   chipset_steps=%llu avg_step_cck=%llu empty_host_steps=%llu\n",
                (unsigned long long)p->multicore.chipset_steps,
                (unsigned long long)avg_step_cck,
                (unsigned long long)p->multicore.empty_host_steps);
        kprintf("[BPROF]   target_cck_max=%llu chipset_cck_max=%llu backlog_avg=%llu backlog_max=%llu\n",
                (unsigned long long)p->multicore.target_cck_max,
                (unsigned long long)p->multicore.chipset_cck_max,
                (unsigned long long)avg_backlog,
                (unsigned long long)p->multicore.backlog_cck_max);
        {
            uint64_t crit_avg = p->multicore.critical_mmio_samples
                              ? p->multicore.critical_mmio_backlog_total /
                                p->multicore.critical_mmio_samples
                              : 0;
            kprintf("[BPROF]   critical_mmio r=%llu w=%llu samples=%llu caught_up=%llu backlog_avg=%llu backlog_max=%llu\n",
                    (unsigned long long)p->multicore.critical_mmio_reads,
                    (unsigned long long)p->multicore.critical_mmio_writes,
                    (unsigned long long)p->multicore.critical_mmio_samples,
                    (unsigned long long)p->multicore.critical_mmio_caught_up,
                    (unsigned long long)crit_avg,
                    (unsigned long long)p->multicore.critical_mmio_backlog_max);
            kprintf("[BPROF]   beam_fast vposr=%llu vhposr=%llu fallback_vposr=%llu fallback_vhposr=%llu snapshot_miss=%llu\n",
                    (unsigned long long)p->multicore.beam_vposr_fast,
                    (unsigned long long)p->multicore.beam_vhposr_fast,
                    (unsigned long long)p->multicore.beam_vposr_fallback,
                    (unsigned long long)p->multicore.beam_vhposr_fallback,
                    (unsigned long long)p->multicore.beam_snapshot_miss);
            kprintf("[BPROF]   posted queued=%llu applied=%llu full_fallbacks=%llu depth_max=%llu\n",
                    (unsigned long long)p->multicore.posted_writes,
                    (unsigned long long)p->multicore.posted_writes_applied,
                    (unsigned long long)p->multicore.posted_queue_full_fallbacks,
                    (unsigned long long)p->multicore.posted_queue_depth_max);
        }
    }

    kprintf("[BPROF] --- Bridge dispatch reference (no fault overhead) ---\n");
    kprintf("[BPROF] bridge_ref_read  : calls=%llu  avg=%llu cy (%llu ns)  min=%llu  max=%llu\n",
            (unsigned long long)p->bridge_ref_read.calls,
            (unsigned long long)avg_cy(&p->bridge_ref_read),
            (unsigned long long)avg_ns(&p->bridge_ref_read, freq),
            (unsigned long long)(p->bridge_ref_read.calls ? p->bridge_ref_read.cycles_min : 0),
            (unsigned long long)p->bridge_ref_read.cycles_max);
    kprintf("[BPROF] bridge_ref_write : calls=%llu  avg=%llu cy (%llu ns)  min=%llu  max=%llu\n",
            (unsigned long long)p->bridge_ref_write.calls,
            (unsigned long long)avg_cy(&p->bridge_ref_write),
            (unsigned long long)avg_ns(&p->bridge_ref_write, freq),
            (unsigned long long)(p->bridge_ref_write.calls ? p->bridge_ref_write.cycles_min : 0),
            (unsigned long long)p->bridge_ref_write.cycles_max);
    if (p->total_access.calls && (p->bridge_ref_read.calls || p->bridge_ref_write.calls))
    {
        uint64_t emu68_avg = avg_cy(&p->total_access);
        uint64_t bridge_avg = (p->bridge_ref_read.cycles_total + p->bridge_ref_write.cycles_total)
                            / (p->bridge_ref_read.calls + p->bridge_ref_write.calls + 1);
        kprintf("[BPROF] => fault_overhead_estimate = %llu cy (%llu ns)  (emu68_avg - bridge_ref_avg)\n",
                (unsigned long long)(emu68_avg > bridge_avg ? emu68_avg - bridge_avg : 0),
                (unsigned long long)((emu68_avg > bridge_avg ? emu68_avg - bridge_avg : 0)
                                     * 1000000000ull / (freq + 1)));
    }

    kprintf("[BPROF] --- Regions ---\n");

#define DUMP_REGION(name, field) \
    kprintf("[BPROF]   %-12s : calls=%llu  avg=%llu cy\n", \
            name, \
            (unsigned long long)p->field.calls, \
            (unsigned long long)avg_cy(&p->field))

    DUMP_REGION("CIA-A",     region_cia_a);
    DUMP_REGION("CIA-B",     region_cia_b);
    DUMP_REGION("OCS-INTR",  region_ocs_intr);
    DUMP_REGION("OCS-OTHER", region_ocs_other);
    DUMP_REGION("OTHER",     region_other);
#undef DUMP_REGION

    kprintf("[BPROF] --- Hot MMIO (top %d) ---\n", BPROF_TOP_MMIO);
    {
        int selected[BPROF_TOP_MMIO];
        for (int i = 0; i < BPROF_TOP_MMIO; i++)
            selected[i] = -1;

        for (int rank = 0; rank < BPROF_TOP_MMIO; rank++) {
            int best = -1;
            for (int i = 0; i < BPROF_MMIO_SLOTS; i++) {
                int already = 0;
                if (!p->hot[i].used || !p->hot[i].count)
                    continue;
                for (int j = 0; j < rank; j++) {
                    if (selected[j] == i) {
                        already = 1;
                        break;
                    }
                }
                if (already)
                    continue;
                if (best < 0 || p->hot[i].count > p->hot[best].count)
                    best = i;
            }
            if (best < 0)
                break;
            selected[rank] = best;
            kprintf("[BPROF]   %06X : total=%llu r=%llu w=%llu last_pc=%08X\n",
                    (unsigned)p->hot[best].addr,
                    (unsigned long long)p->hot[best].count,
                    (unsigned long long)p->hot[best].reads,
                    (unsigned long long)p->hot[best].writes,
                    (unsigned)p->hot[best].last_pc);
        }
    }

    kprintf("[BPROF] ===================================\n");
}

#endif /* BELLATRIX_PROFILE_ENABLED */
