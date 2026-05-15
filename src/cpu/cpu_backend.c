#include "cpu_backend.h"

#include "bridge/bellatrix_bridge.h"

uint32_t cpu_backend_get_pc(CpuBackend *backend)
{
    if (!backend || !backend->get_pc) {
        return 0u;
    }
    return backend->get_pc(backend->ctx);
}

void cpu_backend_set_ipl(CpuBackend *backend, int level)
{
    if (!backend || !backend->set_ipl) {
        return;
    }
    backend->set_ipl(backend->ctx, level);
}

void cpu_backend_reset(CpuBackend *backend)
{
    if (!backend || !backend->reset) {
        return;
    }
    backend->reset(backend->ctx);
}

int cpu_backend_run(CpuBackend *backend, uint32_t cycles)
{
    int used;

    if (!backend || !backend->run) {
        return 0;
    }

    used = backend->run(backend->ctx, cycles);
    if (used > 0) {
        bellatrix_bridge_cpu_progress((uint32_t)used);
    }

    return used;
}
