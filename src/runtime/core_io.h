#ifndef BELLATRIX_RUNTIME_CORE_IO_H
#define BELLATRIX_RUNTIME_CORE_IO_H

#include <stdint.h>
#include <stdbool.h>

#include "machine/machine.h"

#include "io/serial/uart_host.h"
#include "io/bluetooth/bt_host.h"
#include "io/usb/usb_host.h"

typedef struct RuntimeCoreIO {
    BellatrixMachine *machine;

    UARTHost uart_host;
    USBHost usb_host;

    bool running;

    uint64_t local_cycles;
} RuntimeCoreIO;

bool core_io_init(RuntimeCoreIO *core, BellatrixMachine *machine);
void core_io_shutdown(RuntimeCoreIO *core);

void core_io_step(RuntimeCoreIO *core, uint32_t cycles);

bool core_io_open_debug_serial(RuntimeCoreIO *core);

#endif
