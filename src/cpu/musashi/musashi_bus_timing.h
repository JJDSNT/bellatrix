#pragma once

#include <stdint.h>

#include "cpu/cpu_bus_policy.h"

typedef struct MusashiBusTiming {
    const CpuBusPolicy *policy;
    uint32_t published_cycles;
    uint32_t prepublished_cycles;
    unsigned int function_code;
    int run_active;
} MusashiBusTiming;

typedef struct MusashiBusAccessResult {
    uint32_t progress_cycles;
    uint32_t waited_cck;
} MusashiBusAccessResult;

void musashi_bus_timing_init(MusashiBusTiming *timing,
                             const CpuBusPolicy *policy);
void musashi_bus_timing_begin_run(MusashiBusTiming *timing);
uint32_t musashi_bus_timing_sync(MusashiBusTiming *timing);
MusashiBusAccessResult musashi_bus_timing_access(MusashiBusTiming *timing,
                                                 unsigned int size,
                                                 int is_write);
void musashi_bus_timing_end_run(MusashiBusTiming *timing);
