#include "runtime/core_cpu.h"

#include <string.h>

#include "cpu/cpu_backend.h"

static void core_cpu_publish_interrupts(RuntimeCoreCPU *core)
{
    if (!core || !core->machine) {
        return;
    }

    CpuBackend *be = core->machine->cpu_backend;
    if (be && be->set_ipl)
        be->set_ipl(be->ctx, (int)core->machine->current_ipl);
}

bool core_cpu_init(RuntimeCoreCPU *core,
                   BellatrixMachine *machine)
{
    if (!core || !machine) {
        return false;
    }

    memset(core, 0, sizeof(*core));

    core->machine = machine;

    core->base.type = RUNTIME_CORE_CPU;
    core->base.name = "CPU";
    core->base.state = RUNTIME_CORE_RUNNING;

    core->local_cycles = 0;
    core->halted = false;

    return true;
}

void core_cpu_shutdown(RuntimeCoreCPU *core)
{
    if (!core) {
        return;
    }

    core->base.state = RUNTIME_CORE_STOPPED;
    core->halted = true;
}

void core_cpu_reset(RuntimeCoreCPU *core)
{
    if (!core) {
        return;
    }

    core->local_cycles = 0;
    core->halted = false;
    core->base.state = RUNTIME_CORE_RUNNING;

    (void)core; /* CPU reset is owned by the host JIT (Emu68) or harness */
}

void core_cpu_step(RuntimeCoreCPU *core,
                   uint32_t cycles)
{
    if (!core || !core->machine) {
        return;
    }

    if (core->base.state != RUNTIME_CORE_RUNNING) {
        return;
    }

    if (core->halted) {
        return;
    }

    core_cpu_publish_interrupts(core);

    /*
     * On the real target (Emu68), the JIT drives the CPU on Core 0.
     * core_cpu_step() is only meaningful for the Musashi harness backend
     * once it grows a step callback. Until then, treat as no-op.
     */
    core->local_cycles += cycles;
}