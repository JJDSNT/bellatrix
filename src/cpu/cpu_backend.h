// src/cpu/cpu_backend.h
//
// CPU backend interface — the machine's only dependency on the CPU.
// The minimal callbacks are still get_pc (debug) and set_ipl (interrupt
// publication), but executable backends may also expose reset/run so the same
// machine/runtime shell can drive different CPU engines.
//
#pragma once

#include "cpu/direct_region.h"

#include <stdint.h>

typedef struct CpuBackend {
    void *ctx;
    uint32_t (*get_pc)(void *ctx);
    void     (*set_ipl)(void *ctx, int level);
    void     (*reset)(void *ctx);
    int      (*run)(void *ctx, uint32_t cycles);
    int      (*map_direct)(void *ctx, const BellatrixDirectRegion *region);
    int      (*unmap_direct)(void *ctx, uint32_t guest_base, uint32_t size);
    int      progress_in_run;
} CpuBackend;

uint32_t cpu_backend_get_pc(CpuBackend *backend);
void     cpu_backend_set_ipl(CpuBackend *backend, int level);
void     cpu_backend_reset(CpuBackend *backend);
int      cpu_backend_run(CpuBackend *backend, uint32_t cycles);
int      cpu_backend_map_direct(CpuBackend *backend,
                                const BellatrixDirectRegion *region);
int      cpu_backend_unmap_direct(CpuBackend *backend,
                                  uint32_t guest_base, uint32_t size);

/* Build-selected backend ownership. This generic layer, not an Emu68 source
 * file, selects and drives Emu68 or Musashi. */
CpuBackend *cpu_backend_selected(void);
void cpu_backend_init_selected(void);
void cpu_backend_log_selected(void);
int cpu_backend_owns_execution_loop(void);
void cpu_backend_run_selected(void);
