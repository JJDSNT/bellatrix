// src/cpu/cpu_backend.h
//
// CPU backend interface — the machine's only dependency on the CPU.
// Two callbacks: get the current PC (debug) and publish the IPL level.
// The Emu68 backend implements these using __m68k_state + PAL_IPL_Set.
// The Musashi harness backend implements them using m68k_get_reg + m68k_set_irq.

#pragma once
#include <stdint.h>

typedef struct CpuBackend {
    void *ctx;
    uint32_t (*get_pc)(void *ctx);
    void     (*set_ipl)(void *ctx, int level);
} CpuBackend;
