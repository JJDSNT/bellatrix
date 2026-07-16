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
    uint64_t last_dispatch_tick;
    uint64_t dispatch_total_ticks;
    uint64_t dispatch_max_ticks;
    uint64_t dispatch_max_late_ticks;
    uint64_t usb_max_ticks;
    uint64_t bluetooth_max_ticks;
    uint64_t serial_max_ticks;
    uint64_t console_max_ticks;
    uint32_t dispatch_over_budget;
} RuntimeCoreIO;

typedef enum CoreIOEvent {
    CORE_IO_EVENT_USB       = 1u << 0,
    CORE_IO_EVENT_BLUETOOTH = 1u << 1,
    CORE_IO_EVENT_SERIAL    = 1u << 2,
    CORE_IO_EVENT_CONSOLE   = 1u << 3,
    CORE_IO_EVENT_POLL      = CORE_IO_EVENT_USB | CORE_IO_EVENT_BLUETOOTH |
                              CORE_IO_EVENT_SERIAL | CORE_IO_EVENT_CONSOLE
} CoreIOEvent;

typedef struct CoreIOReactorStats {
    uint64_t dispatch_calls;
    uint64_t total_ticks;
    uint64_t max_ticks;
    uint64_t max_late_ticks;
    uint64_t usb_max_ticks;
    uint64_t bluetooth_max_ticks;
    uint64_t serial_max_ticks;
    uint64_t console_max_ticks;
    uint32_t over_budget;
    uint32_t pending_events;
} CoreIOReactorStats;

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

void core_io_step(RuntimeCoreIO *core, uint64_t now, uint64_t freq);
void core_io_notify(uint32_t events);
void core_io_reactor_get_stats(const RuntimeCoreIO *core,
                               CoreIOReactorStats *stats_out);
void core_io_reactor_reset_stats(RuntimeCoreIO *core);

bool core_io_open_debug_serial(RuntimeCoreIO *core);

/* Cross-core serial bridge. The chipset role owns Paula and produces TX;
 * only the host-reactor role touches the physical UART. RX travels in the
 * opposite direction, so the reactor never accesses Rigel internals. All are
 * non-blocking; overflow is counted explicitly. */
bool core_io_serial_enqueue_tx(uint8_t byte);
bool core_io_serial_dequeue_rx(uint8_t *byte_out);
void core_io_serial_get_stats(CoreIOSerialStats *stats_out);

#endif
