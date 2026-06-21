// src/cpu/cpu_bridge.c

#include "cpu/cpu_bridge.h"
#include "cpu/emu68/bellatrix_profile.h"

#include "debug/core_log.h"
#include "machine/machine.h"
#include "machine/memory/memory.h"
#include "runtime/cpu_progress.h"
#include "runtime/core_chipset.h"
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
    core_chipset_lock_acquire();
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
#if defined(BELLATRIX_CORE_LOG)
    cpu_bridge_log_critical_write(addr, value);
#endif
    core_chipset_lock_acquire();
    if (bellatrix_slow_contains(bellatrix_machine_memory(), addr, size))
        bellatrix_machine_write(addr, value, size);
    else
        bellatrix_machine_write(bellatrix_bridge_normalize_addr(addr), value, size);
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
