#include "cpu/cpu_backend.h"

#include "cpu/cpu_bridge.h"
#include "cpu/emu68/bellatrix.h"
#include "cpu/emu68/emu68_backend.h"
#include "cpu/musashi/musashi_backend.h"
#include "host/pal.h"
#include "runtime/core_chipset.h"
#include "support.h"

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
    if (used > 0 && !backend->progress_in_run) {
        bellatrix_bridge_publish_cpu_cycles((uint32_t)used);
    }

    return used;
}

int cpu_backend_map_direct(CpuBackend *backend,
                           const BellatrixDirectRegion *region)
{
    if (!backend || !backend->map_direct)
        return -1;
    return backend->map_direct(backend->ctx, region);
}

int cpu_backend_unmap_direct(CpuBackend *backend,
                             uint32_t guest_base, uint32_t size)
{
    if (!backend || !backend->unmap_direct)
        return -1;
    return backend->unmap_direct(backend->ctx, guest_base, size);
}

CpuBackend *cpu_backend_selected(void)
{
#if defined(BELLATRIX_USE_MUSASHI_CPU) && BELLATRIX_USE_MUSASHI_CPU
    return bellatrix_musashi_backend_get();
#else
    return bellatrix_emu68_backend_get();
#endif
}

void cpu_backend_init_selected(void)
{
#if defined(BELLATRIX_USE_MUSASHI_CPU) && BELLATRIX_USE_MUSASHI_CPU
    bellatrix_musashi_backend_init();
#else
    bellatrix_emu68_backend_init();
#endif
}

void cpu_backend_log_selected(void)
{
#if defined(BELLATRIX_USE_MUSASHI_CPU) && BELLATRIX_USE_MUSASHI_CPU
    kprintf("[BELA] CPU backend: musashi (%s)\n",
            bellatrix_musashi_cpu_model());
#else
    kprintf("[BELA] CPU backend: emu68\n");
#endif
}

int cpu_backend_owns_execution_loop(void)
{
    CpuBackend *backend = cpu_backend_selected();
    return backend && backend->run && backend->reset;
}

void cpu_backend_run_selected(void)
{
    CpuBackend *backend = cpu_backend_selected();

    if (!backend || !backend->run || !backend->reset)
        return;

    cpu_backend_reset(backend);
    for (;;) {
        uint32_t ran;
        uint32_t cycles;

        if (PAL_Core_IsMulticoreEnabled())
            cpu_backend_set_ipl(backend, (int)core_chipset_get_pending_ipl());

        ran = (uint32_t)cpu_backend_run(backend, 454u);
        cycles = ran > 0u ? ran : 454u;
        if (backend->progress_in_run)
            continue;
        if (PAL_Core_IsMulticoreEnabled()) {
            bellatrix_bridge_publish_cpu_cycles(cycles);
        } else {
#if defined(BELLATRIX_HARNESS) && BELLATRIX_HARNESS
            bellatrix_bridge_publish_cpu_cycles(cycles);
#else
            bellatrix_machine_advance_cpu_cycles(cycles);
            PAL_Runtime_Poll();
#endif
        }
    }
}
