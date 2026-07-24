// src/cpu/cpu_bus_policy.c
//
// Institutionalised CpuBusPolicy profiles — the CPU-side counterpart to Rigel's
// cycle_exact mode. See cpu_bus_policy.h and ISSUE-0072. Unwired contract for
// the CPU-dependent corrections (the points behind Battle Squadron's 68000-vs-
// 68040 divergence); each field is applied and validated per step, starting
// from the 68000.

#include "cpu/cpu_bus_policy.h"

#include <string.h>

/* PAL 68000 clock (7.09 MHz) — the timing-test reference config's CPU speed. */
#define CPU_HZ_68000_PAL  7093790u

const CpuBusPolicy cpu_bus_policy_68000 = {
    .name                    = "68000",
    .model                   = CPU_MODEL_68000,
    .cpu_clock_hz            = CPU_HZ_68000_PAL,
    /* Musashi consumes Rigel's slot-owner signal before Chip RAM transfers. */
    .stalls_on_chip_access   = true,
    .irq_recognition_latency = 0u,
};

const CpuBusPolicy cpu_bus_policy_68040 = {
    .name                    = "68040",
    .model                   = CPU_MODEL_68040,
    /* Board-dependent; 0 keeps the current fixed ratio until calibrated. */
    .cpu_clock_hz            = 0u,
    .stalls_on_chip_access   = false,
    .irq_recognition_latency = 0u,
};

const CpuBusPolicy *cpu_bus_policy_by_name(const char *name)
{
    if (name != NULL) {
        if (strcmp(name, "68000") == 0) {
            return &cpu_bus_policy_68000;
        }
        if (strcmp(name, "68040") == 0) {
            return &cpu_bus_policy_68040;
        }
    }
    return &cpu_bus_policy_68040;   /* Emu68's class and the historical default */
}

const CpuBusPolicy *cpu_bus_policy_for_model(CpuModel model)
{
    return model == CPU_MODEL_68000 ? &cpu_bus_policy_68000
                                     : &cpu_bus_policy_68040;
}
