// src/cpu/cpu_bridge.c

#include "cpu/cpu_bridge.h"
#include "cpu/mmio_policy.h"
#include "cpu/emu68/bellatrix.h"
#include "cpu/emu68/bellatrix_profile.h"

#include "debug/core_log.h"
#include "machine/machine.h"
#include "machine/memory/memory.h"
#include "runtime/cpu_progress.h"
#include "runtime/core_chipset.h"
#include "host/pal.h"
#include <stdint.h>

/* Critical custom-chip registers where "CPU wrote it but chipset saw it
 * late/never" bugs would show up. Logged on write only, filtered to this
 * allow-list — these registers are written far too often to log
 * unconditionally without flooding output and perturbing timing. See
 * AI_context/issue_multicore_boundary_logging.md. */
#if defined(BELLATRIX_CORE_LOG)
static void cpu_bridge_log_critical_write(uint32_t addr, uint32_t value)
{
    uint32_t reg = bellatrix_bridge_normalize_addr(addr) & 0x1FFu;

    switch (reg) {
    case 0x058u: XCORE_LOG("CPU->CHIPSET", "BLTSIZE write value=0x%04x", (unsigned)value); break;
    case 0x088u: XCORE_LOG("CPU->CHIPSET", "COPJMP1 write"); break;
    case 0x08Au: XCORE_LOG("CPU->CHIPSET", "COPJMP2 write"); break;
    case 0x096u: XCORE_LOG("CPU->CHIPSET", "DMACON write value=0x%04x", (unsigned)value); break;
    case 0x09Au: XCORE_LOG("CPU->CHIPSET", "INTENA write value=0x%04x", (unsigned)value); break;
    case 0x09Cu: XCORE_LOG("CPU->CHIPSET", "INTREQ write value=0x%04x", (unsigned)value); break;
    default: break;
    }
}
#endif

/* Weak no-op fallback for build configurations that don't link
 * runtime/core_chipset.c (e.g. tools/harness, which drives the machine
 * synchronously and has no separate chipset core). The bare-metal raspi3
 * build links core_chipset.c, whose strong definitions override these. */
__attribute__((weak)) void core_chipset_lock_acquire(void) {}
__attribute__((weak)) void core_chipset_lock_release(void) {}
__attribute__((weak)) void core_chipset_wait_caught_up(void) {}
__attribute__((weak)) void core_chipset_publish_hot_regs(void) {}
__attribute__((weak)) void core_chipset_drain_posted_writes(void) {}
__attribute__((weak)) bool core_chipset_post_write(uint32_t addr,
                                                   uint32_t value,
                                                   uint32_t size)
{
    (void)addr;
    (void)value;
    (void)size;
    return false;
}
__attribute__((weak)) bool core_chipset_read_hot_reg(uint32_t normalized_addr,
                                                     uint32_t *value)
{
    (void)normalized_addr;
    (void)value;
    return false;
}
__attribute__((weak)) uint8_t core_chipset_get_pending_ipl(void) { return 0u; }
__attribute__((weak)) void core_chipset_set_pending_ipl(uint8_t ipl) { (void)ipl; }
__attribute__((weak)) bool core_chipset_get_progress(uint64_t *chipset_cck,
                                                     uint64_t *target_cck)
{
    (void)chipset_cck;
    (void)target_cck;
    return false;
}

#if BELLATRIX_PROFILE_ENABLED || defined(BELLATRIX_ENABLE_MULTICORE)
static int cpu_bridge_is_critical_mmio(uint32_t addr, unsigned int size,
                                       int is_write)
{
    BellatrixMmioPolicy policy = bellatrix_mmio_policy(
        bellatrix_bridge_normalize_addr(addr), size, is_write);
    return policy == BELLATRIX_MMIO_SYNC ||
           policy == BELLATRIX_MMIO_PUBLISHED;
}
#endif /* PROFILE || MULTICORE */

#if BELLATRIX_PROFILE_ENABLED
static void cpu_bridge_profile_critical_mmio(uint32_t addr, unsigned int size,
                                             int is_write)
{
    uint64_t chipset_cck = 0;
    uint64_t target_cck = 0;

    if (!PAL_Core_IsMulticoreEnabled())
        return;
    if (!cpu_bridge_is_critical_mmio(addr, size, is_write))
        return;
    if (!core_chipset_get_progress(&chipset_cck, &target_cck))
        return;

    bprof_multicore_critical_mmio(addr, is_write, chipset_cck, target_cck);
}
#endif

/*
 * Routing of a 32-bit CPU-space access above the 24-bit Amiga bus.
 *
 *   DIRECT   - RAM/ROM board backing. Installed in the backend (Emu68 MMU /
 *              Musashi bank) and consumed BEFORE the bridge, so it never
 *              reaches here.
 *   EXTERNAL - a configured Z3 board register window, decoded by the Super
 *              Buster. Routed to its owner with the FULL address.
 *   UNMAPPED - open bus. Never mask the address down to 24 bits: a probe at
 *              0xFF000000+ masked to 0x000000+ aliased into live chip RAM and
 *              the RTG DiagArea, producing non-deterministic boot behaviour
 *              (found 2026-07-03 chasing a run-to-run-varying boot hang).
 */
enum cpu_bridge_route {
    CPU_BRIDGE_ROUTE_AMIGA_LOW = 0, /* <= 0x00FFFFFF: normal machine dispatch */
    CPU_BRIDGE_ROUTE_Z3_EXTERNAL,   /* configured Z3 board window (full addr) */
    CPU_BRIDGE_ROUTE_OPEN_BUS       /* unmapped 32-bit space */
};

static inline enum cpu_bridge_route cpu_bridge_classify(uint32_t addr)
{
    if (addr <= 0x00FFFFFFu)
        return CPU_BRIDGE_ROUTE_AMIGA_LOW;
    return bellatrix_machine_z3_external_owns(addr) ?
               CPU_BRIDGE_ROUTE_Z3_EXTERNAL : CPU_BRIDGE_ROUTE_OPEN_BUS;
}

static inline uint32_t cpu_bridge_open_bus(unsigned int size)
{
    return (size == 1) ? 0xFFu : (size == 2) ? 0xFFFFu : 0xFFFFFFFFu;
}

uint32_t bellatrix_bridge_normalize_addr(uint32_t addr)
{
    addr &= 0x00FFFFFFu;

    /*
     * Emu68 / ROM code may hit mirrored custom-chip addresses instead of the
     * canonical 0xDFFxxx window. Collapse the observed 0x00C?Fxxx mirror so
     * every CPU adapter sees the same logical register page.
     */
    if ((addr & 0x00F00000u) == 0x00C00000u) {
        if ((addr & 0x0000F000u) == 0x0000F000u)
            addr = 0x00DFF000u | (addr & 0x00000FFFu);
    }

    return addr;
}

uint32_t bellatrix_bridge_cpu_read(uint32_t addr, unsigned int size)
{
#if BELLATRIX_PROFILE_ENABLED
    uint64_t _t = bprof_now();
#endif
    uint32_t result;
    enum cpu_bridge_route route = cpu_bridge_classify(addr);

    if (route != CPU_BRIDGE_ROUTE_AMIGA_LOW) {
        if (route == CPU_BRIDGE_ROUTE_OPEN_BUS)
            return cpu_bridge_open_bus(size);
        /* Z3 EXTERNAL board register window: route the full address to its
         * owner (Super Buster -> Z3 slot). No 24-bit normalisation; ordering
         * with posted writes is preserved under the chipset lock. */
        core_chipset_lock_acquire();
        core_chipset_drain_posted_writes();
        result = bellatrix_machine_read(addr, size);
        core_chipset_lock_release();
#if BELLATRIX_PROFILE_ENABLED
        bprof_record(&g_bprof.bridge_ref_read, bprof_now() - _t);
#endif
        return result;
    }

#if defined(BELLATRIX_ENABLE_MULTICORE)
    /* Hot poll registers are served from Core 2 published state with no
     * rendezvous and no lock. Beam registers use f(cpu_time) over a geometry
     * snapshot (PiStorm-housekeeper pattern; ISSUE-0049). */
    if (size == 2u &&
        core_chipset_read_hot_reg(bellatrix_bridge_normalize_addr(addr),
                                  &result))
        return result;
#endif
#if BELLATRIX_PROFILE_ENABLED
    cpu_bridge_profile_critical_mmio(addr, size, 0);
#endif
#if defined(BELLATRIX_ENABLE_MULTICORE)
    /* Critical read must see fresh chipset state: let Core 2 catch up first. */
    if (cpu_bridge_is_critical_mmio(addr, size, 0))
        core_chipset_wait_caught_up();
#endif
    core_chipset_lock_acquire();
    core_chipset_drain_posted_writes();
    if (bellatrix_slow_contains(bellatrix_machine_memory(), addr, size))
        result = bellatrix_machine_read(addr, size);
    else
        result = bellatrix_machine_read(bellatrix_bridge_normalize_addr(addr), size);
    core_chipset_lock_release();
#if BELLATRIX_PROFILE_ENABLED
    bprof_record(&g_bprof.bridge_ref_read, bprof_now() - _t);
#endif
    return result;
}

void bellatrix_bridge_cpu_write(uint32_t addr, uint32_t value, unsigned int size)
{
#if BELLATRIX_PROFILE_ENABLED
    uint64_t _t = bprof_now();
#endif
    /* Emu68's native 68040 support ROM uses this byte-wide diagnostic port.
     * The original vectors.c owns it for the JIT/fault path; Musashi reaches
     * the same ARM-side semantic through the sparse bridge instead. */
    if (addr == 0xdeadbeefu && size == 1u) {
        kprintf("%c", (int)(value & 0xffu));
        return;
    }
    {
        enum cpu_bridge_route route = cpu_bridge_classify(addr);
        if (route != CPU_BRIDGE_ROUTE_AMIGA_LOW) {
            if (route == CPU_BRIDGE_ROUTE_OPEN_BUS)
                return;
            /* Z3 EXTERNAL board register window: full-address write to its
             * owner, drained under the chipset lock. */
            core_chipset_lock_acquire();
            core_chipset_drain_posted_writes();
            bellatrix_machine_write(addr, value, size);
            core_chipset_lock_release();
#if BELLATRIX_PROFILE_ENABLED
            bprof_record(&g_bprof.bridge_ref_write, bprof_now() - _t);
#endif
            return;
        }
    }

#if defined(BELLATRIX_CORE_LOG)
    cpu_bridge_log_critical_write(addr, value);
#endif
#if BELLATRIX_PROFILE_ENABLED
    cpu_bridge_profile_critical_mmio(addr, size, 1);
#endif
#if defined(BELLATRIX_ENABLE_MULTICORE)
    /* Non-critical custom-register writes are posted with the CPU's emulated
     * time and applied by Core 2 exactly at that stamp — no lock and no
     * rendezvous here (PiStorm write-buffer pattern, ISSUE-0048). Ordering
     * with everything observable is preserved: reads and critical writes
     * drain the queue under the lock before acting. */
    {
        uint32_t norm = bellatrix_bridge_normalize_addr(addr);
        if (norm >= 0x00DFF000u && norm <= 0x00DFF1FFu &&
            !cpu_bridge_is_critical_mmio(addr, size, 1) &&
            !bellatrix_slow_contains(bellatrix_machine_memory(), addr, size) &&
            core_chipset_post_write(norm, value, size))
            return;
    }

    /* Critical write must land at the CPU's time: let Core 2 catch up first so
     * it applies at the right beam position, not up to a backlog window late. */
    if (cpu_bridge_is_critical_mmio(addr, size, 1))
        core_chipset_wait_caught_up();
#endif
    core_chipset_lock_acquire();
    core_chipset_drain_posted_writes();
    if (bellatrix_slow_contains(bellatrix_machine_memory(), addr, size))
        bellatrix_machine_write(addr, value, size);
    else
        bellatrix_machine_write(bellatrix_bridge_normalize_addr(addr), value, size);
#if defined(BELLATRIX_ENABLE_MULTICORE)
    /* Read-own-write consistency for the hot-register snapshot: a critical
     * write (INTENA/INTREQ/DMACON, ...) must be visible to the very next
     * lock-free hot read, not only after Core 2's next drain iteration. */
    if (cpu_bridge_is_critical_mmio(addr, size, 1))
        core_chipset_publish_hot_regs();
#endif
    core_chipset_lock_release();
#if BELLATRIX_PROFILE_ENABLED
    bprof_record(&g_bprof.bridge_ref_write, bprof_now() - _t);
#endif
}

uint32_t bellatrix_bridge_cpu_access(uint32_t addr,
                                     uint32_t value,
                                     unsigned int size,
                                     int dir)
{
    if (dir == BELLATRIX_BUS_WRITE) {
        bellatrix_bridge_cpu_write(addr, value, size);
        return 0;
    }

    return bellatrix_bridge_cpu_read(addr, size);
}

void bellatrix_bridge_publish_cpu_cycles(uint32_t cycles)
{
    bellatrix_runtime_publish_cpu_cycles(cycles);
}

void bellatrix_bridge_cpu_progress(uint32_t cycles)
{
    if (cycles == 0u)
        return;

    /*
     * A progress notification issued from inside a CPU timeslice must follow
     * the same ownership rule as cpu_backend_run_selected(): Core 2 consumes
     * the published horizon in multicore mode, while single-core has no
     * consumer and therefore advances Rigel synchronously.
     */
    if (PAL_Core_IsMulticoreEnabled()) {
        bellatrix_bridge_publish_cpu_cycles(cycles);
    } else {
#if defined(BELLATRIX_HARNESS) && BELLATRIX_HARNESS
        bellatrix_machine_advance(cycles);
#else
        bellatrix_machine_advance_cpu_cycles(cycles);
#endif
        PAL_Runtime_Poll();
    }
}

void bellatrix_bridge_cpu_sync_ipl(void)
{
    bellatrix_machine_sync_ipl();
}
