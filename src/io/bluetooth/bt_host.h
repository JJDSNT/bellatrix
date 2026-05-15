#ifndef BELLATRIX_IO_BT_HOST_H
#define BELLATRIX_IO_BT_HOST_H

#include <stdbool.h>
#include <stdint.h>

typedef struct BTHost {
    bool enabled;
    bool initialized;
    bool pairing_window_open;
    bool phase1_complete;
    bool hci_ready;
    uint32_t baudrate;
    uint32_t pairing_window_ms;
    uint8_t bootstrap_state;
    uint8_t power_cycle_attempts;
    uint32_t bootstrap_deadline_ms;
    uint32_t init_deadline_ms;
} BTHost;

/**
 * Initialize Bluetooth host subsystem.
 * Called during system boot (Core 3 initialization).
 */
bool bt_host_init(BTHost *bt);
bool bt_host_wait_for_bootstrap(BTHost *bt, uint32_t timeout_ms);

/**
 * Process Bluetooth stack events and timers.
 * Should be called periodically from Core 3 step.
 */
void bt_host_step(BTHost *bt);

/**
 * Shutdown Bluetooth subsystem.
 */
void bt_host_shutdown(BTHost *bt);

#endif // BELLATRIX_IO_BT_HOST_H
