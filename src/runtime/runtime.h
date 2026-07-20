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

/* The machine-wide runtime instance. Defined by the machine bring-up in
 * src/cpu/emu68/bellatrix.c. Declared here so consumers stop reaching for it
 * with hand-written externs (core_io.c carried three of its own). */
extern BellatrixRuntime g_runtime;

/* Platform service point. Multicore calls this from the host reactor;
 * single-core calls it cooperatively through PAL_Runtime_Poll(). */
void bellatrix_runtime_io_step(uint64_t now, uint64_t freq);

/* Rate-limited (~1 kHz) wrapper over bellatrix_runtime_io_step, for callers
 * that service I/O from their own loop instead of the host reactor — the
 * launcher during boot, before Core 3 takes the reactor over. Despite its
 * former name (bellatrix_launcher_pump_usb) this drives the whole I/O
 * reactor, USB and Bluetooth alike. */
void bellatrix_runtime_io_pump(void);

#endif
