#ifndef BELLATRIX_IO_BT_HOST_H
#define BELLATRIX_IO_BT_HOST_H

#include <stdbool.h>
#include <stdint.h>

typedef struct BTHost {
    bool enabled;
    bool initialized;
    uint32_t baudrate;
} BTHost;

/**
 * Initialize Bluetooth host subsystem.
 * Called during system boot (Core 3 initialization).
 */
bool bt_host_init(BTHost *bt);

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
