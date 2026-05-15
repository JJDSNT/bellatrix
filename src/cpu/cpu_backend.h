// src/cpu/cpu_backend.h
//
// CPU backend interface — the machine's only dependency on the CPU.
// The minimal callbacks are still get_pc (debug) and set_ipl (interrupt
// publication), but executable backends may also expose reset/run so the same
// machine/runtime shell can drive different CPU engines.
//
// The Emu68 backend currently implements only get_pc + set_ipl because the
// JIT owns the real execution loop in baremetal.
// The Musashi backend implements all four callbacks.

#pragma once

#include <stdint.h>

typedef struct CpuBackend {
    void *ctx;
    uint32_t (*get_pc)(void *ctx);
    void     (*set_ipl)(void *ctx, int level);
    void     (*reset)(void *ctx);
    int      (*run)(void *ctx, uint32_t cycles);
} CpuBackend;

uint32_t cpu_backend_get_pc(CpuBackend *backend);
void     cpu_backend_set_ipl(CpuBackend *backend, int level);
void     cpu_backend_reset(CpuBackend *backend);
int      cpu_backend_run(CpuBackend *backend, uint32_t cycles);
