#include "runtime/core_cpu.h"

#include <string.h>

#include "cpu/cpu_backend.h"
#include "debug/core_log.h"

static void core_cpu_publish_interrupts(RuntimeCoreCPU *core)
{
    if (!core || !core->machine) {
        return;
    }

    CpuBackend *be = core->machine->cpu_backend;
    if (be) {
        uint8_t ipl = (uint8_t)core->machine->current_ipl;
        cpu_backend_set_ipl(be, (int)ipl);
        if (ipl != core->last_ipl) {
            XCORE_LOG("PAULA->CPU", "IPL %u -> %u", (unsigned)core->last_ipl, (unsigned)ipl);
            core->last_ipl = ipl;
        }
    }
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
    core->last_ipl = 0;

    CPU_LOG("init");
    return true;
}

void core_cpu_shutdown(RuntimeCoreCPU *core)
{
    if (!core) {
        return;
    }

    CPU_LOG("shutdown cycles=%llu", (unsigned long long)core->local_cycles);
    core->base.state = RUNTIME_CORE_STOPPED;
    core->halted = true;
}

void core_cpu_reset(RuntimeCoreCPU *core)
{
    if (!core) {
        return;
    }

    CPU_LOG("reset");
    core->local_cycles = 0;
    core->last_ipl = 0;
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

    if (core->machine->cpu_backend && core->machine->cpu_backend->run) {
        int used = cpu_backend_run(core->machine->cpu_backend, cycles);
        core->local_cycles += (uint64_t)used;
        if (used > 0 && !core->machine->cpu_backend->progress_in_run)
            bellatrix_bridge_publish_cpu_cycles((uint32_t)used);
        return;
    }

    /* Emu68 still owns the real baremetal loop. */
    core->local_cycles += cycles;
}
