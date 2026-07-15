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
    uint32_t connection_deadline_ms;
    uint16_t pending_hid_cid;
    uint8_t connection_state;
    uint8_t recovery_direct_attempts;
    uint8_t hid_activation_stage;
    uint16_t hid_activation_cid;
    bool discovery_started;
    bool mouse_connected;
    bool mouse_input_ready;
    bool hid_connect_pending;
    bool outgoing_reconnect_suspended;
    uint8_t link_recovery_state;
    uint8_t link_recovery_reason;
    uint8_t link_recovery_error_code;
    uint8_t last_hid_status;
    uint32_t hid_reports_received;
} BTHost;

enum {
    BT_CONNECTION_WAIT_STACK = 0,
    BT_CONNECTION_PASSIVE,
    BT_CONNECTION_CONNECTING,
    BT_CONNECTION_DISCOVERING,
    BT_CONNECTION_ACTIVE,
    BT_CONNECTION_BACKOFF,
};

/**
 * Initialize Bluetooth host subsystem.
 * Called during system boot (Core 3 initialization).
 */
bool bt_host_init(BTHost *bt);
bool bt_host_wait_for_bootstrap(BTHost *bt, uint32_t timeout_ms);

/* True once the controller reached HCI WORKING (bootstrap succeeded). */
bool bt_host_is_working(const BTHost *bt);

/* Connect to all pairs currently in bt_pairs (call after launcher loads BTPAIRS.TXT). */
void bt_host_connect_pairs(BTHost *bt);
void bt_host_close_pairing_window(BTHost *bt);
void bt_host_open_pairing_window(BTHost *bt);
void bt_host_set_outgoing_reconnect_suspended(BTHost *bt, bool suspended);
void bt_host_prepare_explicit_pairing(BTHost *bt, const uint8_t addr[6]);
bool bt_host_mouse_connected(void);
uint8_t bt_host_last_hid_status(void);
bool bt_host_recovery_discovery_active(const BTHost *bt);
void bt_host_claim_recovery_discovery(BTHost *bt);

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
