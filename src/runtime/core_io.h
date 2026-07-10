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

typedef struct CoreIOSerialStats {
    uint32_t tx_depth;
    uint32_t tx_max_depth;
    uint32_t tx_dropped;
    uint32_t rx_depth;
    uint32_t rx_max_depth;
    uint32_t rx_dropped;
} CoreIOSerialStats;

bool core_io_init(RuntimeCoreIO *core, BellatrixMachine *machine);
void core_io_shutdown(RuntimeCoreIO *core);

void core_io_step(RuntimeCoreIO *core, uint32_t cycles);

bool core_io_open_debug_serial(RuntimeCoreIO *core);

/* Cross-core serial bridge.  Core 2 owns Paula and produces TX bytes; Core 3
 * alone touches the physical UART. RX travels in the opposite direction so
 * Core 3 never needs to access the Rigel context. All operations are
 * non-blocking; overflow is counted explicitly. */
bool core_io_serial_enqueue_tx(uint8_t byte);
bool core_io_serial_dequeue_rx(uint8_t *byte_out);
void core_io_serial_get_stats(CoreIOSerialStats *stats_out);

#endif
