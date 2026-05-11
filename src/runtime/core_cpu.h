#ifndef BELLATRIX_RUNTIME_CORE_CPU_H
#define BELLATRIX_RUNTIME_CORE_CPU_H

#include <stdint.h>
#include <stdbool.h>

#include "core/machine.h"
#include "runtime/runtime_core.h"

typedef struct RuntimeCoreCPU {
    RuntimeCore base;

    BellatrixMachine *machine;

    uint64_t local_cycles;

    bool halted;
} RuntimeCoreCPU;

bool core_cpu_init(RuntimeCoreCPU *core,
                   BellatrixMachine *machine);

void core_cpu_shutdown(RuntimeCoreCPU *core);

void core_cpu_step(RuntimeCoreCPU *core,
                   uint32_t cycles);

void core_cpu_reset(RuntimeCoreCPU *core);

#endif