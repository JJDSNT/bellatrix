#ifndef BELLATRIX_RUNTIME_RUNTIME_H
#define BELLATRIX_RUNTIME_RUNTIME_H

#include <stdint.h>
#include <stdbool.h>

#include "machine/machine.h"

#include "runtime/core_cpu.h"
#include "runtime/core_chipset.h"
#include "runtime/core_io.h"
#include "io/bluetooth/bt_host.h"

typedef struct BellatrixRuntime {
    BellatrixMachine *machine;

    RuntimeCoreCPU     cpu;
    RuntimeCoreChipset chipset;
    RuntimeCoreIO      io;

    BTHost bluetooth;

    bool running;

    uint32_t cycles_per_step;
} BellatrixRuntime;

bool bellatrix_runtime_init(
    BellatrixRuntime *rt,
    BellatrixMachine *machine);

void bellatrix_runtime_shutdown(
    BellatrixRuntime *rt);

void bellatrix_runtime_reset(
    BellatrixRuntime *rt);

void bellatrix_runtime_run(
    BellatrixRuntime *rt);

void bellatrix_runtime_stop(
    BellatrixRuntime *rt);

#endif
