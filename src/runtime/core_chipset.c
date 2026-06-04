// src/runtime/core_chipset.c
//
// Core 1 — Rigel chipset domain.
//
// Owns the full chipset tick (Agnus, Denise, Paula, CIA) via rigel_step().
// Runs on Core 1 in multicore mode; driven from the single-core poll path
// via bellatrix_runtime_host_step() when multicore is disabled.
//
// Synchronisation with Core 0 (CPU):
//   - Core 0 calls bellatrix_runtime_notify_cpu_progress() after each JIT
//     block, publishing M68K cycles converted to CCK into s_cpu_cck_target.
//   - Core 1 wakes on SEV, drains cycles until caught up, then WFEs again.
//   - IPL changes and frame events are published atomically back to Core 0.

#include "runtime/core_chipset.h"

#include <stdatomic.h>
#include <string.h>

#include "rigel/rigel.h"
#include "rigel/rigel_irq.h"
#include "machine/machine.h"
#include "debug/core_log.h"
#include "host/pal.h"

/* Rigel step granularity in CCK cycles. */
#define CHIPSET_QUANTUM 128u

/* Published by Core 0; consumed by Core 1. */
static _Atomic uint64_t s_cpu_cck_target = 0;

/* Local to Core 1 — never written by Core 0. */
static uint64_t s_chipset_cck = 0;

/* Remainder for M68K→CCK conversion (local to Core 0 call site). */
static uint32_t s_m68k_rem = 0;

static RuntimeCoreChipset *s_core = NULL;

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
    s_chipset_cck = 0;
    s_m68k_rem    = 0;

    CORE1_LOG("chipset init (Rigel)");
    return true;
}

void core_chipset_shutdown(RuntimeCoreChipset *core)
{
    if (!core) return;
    core->running = false;
    s_core = NULL;
    CORE1_LOG("chipset shutdown cck=%llu", (unsigned long long)core->local_cycles);
}

void core_chipset_reset(RuntimeCoreChipset *core)
{
    if (!core) return;
    core->local_cycles = 0;
    atomic_store_explicit(&s_cpu_cck_target, 0u, memory_order_release);
    s_chipset_cck = 0;
    s_m68k_rem    = 0;
    CORE1_LOG("chipset reset");
}

/* ---------------------------------------------------------------------------
 * Called by Core 0 (CPU / JIT) after each advance quantum.
 * Converts M68K cycles → CCK and signals Core 1.
 * ------------------------------------------------------------------------- */
void bellatrix_runtime_notify_cpu_progress(uint32_t m68k_cycles)
{
    uint32_t total = s_m68k_rem + m68k_cycles;
    uint32_t cck   = total >> 1u;          /* M68K / 2 = CCK */
    s_m68k_rem     = total & 1u;

    if (cck > 0u) {
        atomic_fetch_add_explicit(&s_cpu_cck_target, cck, memory_order_release);
        PAL_Runtime_WakeupChipset();        /* sev — wake Core 1 */
    }
}

/* ---------------------------------------------------------------------------
 * Called by Core 1 from chipset_core_loop() in pal_core.c.
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

    while (s_chipset_cck < target) {
        uint64_t remaining = target - s_chipset_cck;
        uint32_t step = (remaining > CHIPSET_QUANTUM) ? CHIPSET_QUANTUM
                                                      : (uint32_t)remaining;

        rigel_step_result_t r = rigel_step(core->rigel, step);
        s_chipset_cck      += step;
        core->local_cycles += step;
        core->machine->tick_count = (uint64_t)r.time;

        if (r.events & RIGEL_EVENT_IRQ_CHANGED)
            bellatrix_machine_on_ipl_changed(
                (uint8_t)rigel_get_ipl(core->rigel));

        if (r.events & RIGEL_EVENT_FRAME_READY)
            bellatrix_machine_on_frame_ready();
    }

    bellatrix_machine_post_chipset_step();
}
