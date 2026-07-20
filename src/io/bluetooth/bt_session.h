#ifndef BELLATRIX_IO_BT_SESSION_H
#define BELLATRIX_IO_BT_SESSION_H

/*
 * Handle-free Bluetooth control surface for consumers that do not own the
 * stack — today the launcher's scan/pairing screen.
 *
 * bt_host.h is the BTStack host itself and every call there takes a
 * BellatrixBluetooth* handle. This layer exists for two reasons:
 *
 *   1. It resolves the runtime handle (g_runtime.bluetooth) so a consumer
 *      never reaches into machine-global state to talk to Bluetooth.
 *   2. It is a null object when BELLATRIX_ENABLE_BTSTACK is 0: every entry
 *      point is still defined, returning "nothing connected", so consumers
 *      link and run unconditionally with no #ifdef of their own.
 *
 * The launcher consumes HID; it does not own Bluetooth. These functions used
 * to live in the Emu68 CPU adapter as bellatrix_launcher_bt_*, with their
 * prototypes hand-copied into each consumer and declared in no header at all.
 */

#include <stdint.h>

/* Advance the stack. Safe to call at any rate; a no-op without BTStack. */
void bt_session_poll(void);

/* True once the host is initialised and usable. */
int bt_session_ready(void);

/* Connect to every saved pair. _connect_pairs reports whether a mouse came
 * up; _connect_now is the same action when the answer is not needed. */
int  bt_session_connect_pairs(void);
void bt_session_connect_now(void);

int bt_session_mouse_connected(void);

/* Hold off automatic outgoing reconnects (e.g. while the user is pairing). */
void bt_session_suspend_reconnect(int suspended);

int  bt_session_recovery_discovery_active(void);
void bt_session_claim_recovery_discovery(void);

/* Explicit pairing against a specific device. */
void bt_session_prepare_pairing(const uint8_t addr[6]);
void bt_session_open_pairing(void);
void bt_session_close_pairing(void);

#endif /* BELLATRIX_IO_BT_SESSION_H */
