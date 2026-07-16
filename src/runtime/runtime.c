// src/runtime/runtime.c

#include "runtime/runtime.h"

#include <string.h>

#include "debug/core_log.h"
#include "host/pal.h"
#include "runtime/topology.h"
#include "support.h"

extern RigelContext *bellatrix_machine_rigel_ctx(void);

bool bellatrix_runtime_init(
    BellatrixRuntime *rt,
    BellatrixMachine *machine)
{
    if (!rt || !machine) {
        return false;
    }

    memset(rt, 0, sizeof(*rt));

    rt->machine = machine;
    rt->cycles_per_step = 4;

    if (!core_cpu_init(&rt->cpu, machine)) {
        return false;
    }

    RigelContext *rigel = bellatrix_machine_rigel_ctx();
    if (!core_chipset_init(&rt->chipset, rigel, machine)) {
        return false;
    }

    if (!core_io_init(&rt->io, machine)) {
        return false;
    }

    rt->bluetooth.enabled     = false;
    rt->bluetooth.initialized = false;
    rt->running = true;

    if (PAL_Core_IsMulticoreEnabled()) {
        kprintf("[RUNTIME] init OK: cpu=%u chipset=%u host=%u\n",
                (unsigned)BELLATRIX_CORE_CPU,
                (unsigned)BELLATRIX_CORE_CHIPSET,
                (unsigned)BELLATRIX_CORE_HOST_REACTOR);
        PAL_Core_LaunchChipset(NULL);
    } else {
        kprintf("[RUNTIME] init OK: single-core mode\n");
    }

    return true;
}

void bellatrix_runtime_shutdown(
    BellatrixRuntime *rt)
{
    if (!rt) {
        return;
    }

    kprintf("[RUNTIME] shutdown\n");
    core_cpu_shutdown(&rt->cpu);
    core_chipset_shutdown(&rt->chipset);
    core_io_shutdown(&rt->io);
    bt_host_shutdown(&rt->bluetooth);
    rt->running = false;
}

void bellatrix_runtime_reset(
    BellatrixRuntime *rt)
{
    if (!rt || !rt->machine) {
        return;
    }

    kprintf("[RUNTIME] reset\n");
    bellatrix_machine_reset();
    core_cpu_reset(&rt->cpu);
    core_chipset_reset(&rt->chipset);
}

void bellatrix_runtime_run(
    BellatrixRuntime *rt)
{
    if (!rt) {
        return;
    }

    rt->running = true;

    /*
     * In multicore mode the chipset and host-reactor roles run independently
     * of the CPU role. PAL_Runtime_Poll() services the single-core fallback.
     */
    while (rt->running) {
        core_cpu_step(&rt->cpu, rt->cycles_per_step);

        if (!PAL_Core_IsMulticoreEnabled()) {
            PAL_Runtime_Poll();
        }
    }
}

void bellatrix_runtime_stop(
    BellatrixRuntime *rt)
{
    if (!rt) {
        return;
    }

    rt->running = false;
}
