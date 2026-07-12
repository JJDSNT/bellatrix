// src/cpu/cpu_bridge.c

#include "cpu/cpu_bridge.h"
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
    uint32_t normalized;
    uint32_t reg;

    if (size != 1u && size != 2u && size != 4u)
        return 0;

    normalized = bellatrix_bridge_normalize_addr(addr);

    if (normalized >= 0x00BFD000u && normalized <= 0x00BFEFFFu)
        return 1;

    if (normalized < 0x00DFF000u || normalized > 0x00DFF1FFu)
        return 0;

    reg = normalized & 0x1ffu;
    if (is_write) {
        switch (reg) {
        case 0x058u: /* BLTSIZE */
        case 0x088u: /* COPJMP1 */
        case 0x08au: /* COPJMP2 */
        case 0x096u: /* DMACON */
        case 0x09au: /* INTENA */
        case 0x09cu: /* INTREQ */
        case 0x092u: /* DDFSTRT */
        case 0x094u: /* DDFSTOP */
            return 1;
        default:
            return 0;
        }
    }

    switch (reg) {
    case 0x002u: /* DMACONR */
    case 0x004u: /* VPOSR */
    case 0x006u: /* VHPOSR */
    case 0x010u: /* ADKCONR */
    case 0x016u: /* POTINP */
    case 0x01au: /* DSKBYTR */
    case 0x01cu: /* INTENAR */
    case 0x01eu: /* INTREQR — was missing: reads never rendezvoused */
        return 1;
    default:
        return 0;
    }
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

/* Zorro III / 32-bit space (addr > 0x00FFFFFF, e.g. autoconfig at
 * 0xFF000000 — EZ3_EXPANSIONBASE) is not implemented (ISSUE-0032). This
 * MUST read as open bus, not alias into 24-bit space: AROS's Z3 probe
 * during boot reads 0xFF000000+, and masking it down previously landed
 * on live chip RAM (0x000000+) and the RTG board's own DiagArea
 * (0x000100+) — the probe was reading mutable, run-to-run-varying state
 * instead of "nothing here", which produced non-deterministic boot
 * behavior (found 2026-07-03 chasing a boot hang that resolved
 * differently across runs of the same build). */
static inline int addr_is_unimplemented_z3(uint32_t addr)
{
    if (addr <= 0x00FFFFFFu)
        return 0;
    {
        static int s_hits = 0;
        if (s_hits < 20) {
            s_hits++;
            kprintf("[Z3-OPENBUS] addr=%08x (unimplemented — see ISSUE-0032)\n",
                    (unsigned)addr);
        }
    }
    return 1;
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

    if (addr_is_unimplemented_z3(addr))
        return (size == 1) ? 0xFFu : (size == 2) ? 0xFFFFu : 0xFFFFFFFFu;

#if defined(BELLATRIX_ENABLE_MULTICORE)
    /* Hot poll registers are served from the Core 2 published snapshot with
     * no rendezvous and no lock (PiStorm-housekeeper pattern; ISSUE-0048). */
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
    if (addr_is_unimplemented_z3(addr))
        return;

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
    bellatrix_bridge_publish_cpu_cycles(cycles);
}

void bellatrix_bridge_cpu_sync_ipl(void)
{
    bellatrix_machine_sync_ipl();
}
