#ifndef BELLATRIX_IO_BT_HID_H
#define BELLATRIX_IO_BT_HID_H

#include <stdint.h>

/* BT HID report dispatcher.
 * Receives boot-protocol HID reports from the btstack HID host and
 * routes them to the same machine APIs used by USB HID:
 *   keyboard → bellatrix_machine_keyboard_rawkey()  (CIA-A SDR)
 *   mouse    → bellatrix_machine_mouse_motion/button()
 *   joystick → bellatrix_machine_joystick_direction/button()
 *
 * Both USB and BT write to the same accumulators — no arbitration needed. */

/* Called once at startup (before any HID connections). */
void bt_hid_init(void);

/* Process a boot-protocol keyboard report (8 bytes: modifier + reserved + 6 keys).
 * hid_cid identifies which connection this report came from. */
void bt_hid_handle_keyboard_report(uint16_t hid_cid,
                                   const uint8_t *data, uint16_t len);

/* Process a boot-protocol mouse report (≥3 bytes: buttons + xdisp + ydisp). */
void bt_hid_handle_mouse_report(uint16_t hid_cid,
                                const uint8_t *data, uint16_t len);

/* Process a joystick/gamepad HID report in boot or simple report format. */
void bt_hid_handle_joystick_report(uint16_t hid_cid,
                                   const uint8_t *data, uint16_t len);

/* Release all held keys/buttons for a connection (called on disconnect). */
void bt_hid_release_all(uint16_t hid_cid);

#endif /* BELLATRIX_IO_BT_HID_H */
