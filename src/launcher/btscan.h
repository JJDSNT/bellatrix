// src/launcher/btscan.h
// Bluetooth scan / pairing front-end for the launcher.
//
// BTStack owns PL011 while the independent mini-UART keeps diagnostics alive.
// Discovery results are also shown on the framebuffer. This screen observes BT
// state and drives pairing through bt_pairs / bt_scan; it never owns the
// BTstack lifecycle (see ISSUE-0058).
#pragma once
#include <stdbool.h>

#if BELLATRIX_ENABLE_BTSTACK

// Run the scan screen until the user continues or the budget elapses.
// force_scan=false skips discovery when a mouse is already saved.
void btscan_screen(bool force_scan);

// True if BTPAIRS.TXT holds at least one mouse.
bool btscan_has_saved_mouse(void);

// Write BTSCAN.TXT (scan results + bt_diag snapshot) to SD, overwriting in
// place. Also flushes any newly created link keys. Safe to call repeatedly.
void launcher_save_bt_report(void);
// Runtime modal lifecycle. The host reactor calls step after its normal
// pass; discovery/pairing progress remains owned by BTstack.
bool btscan_runtime_open(void);
bool btscan_runtime_step(void); // true while the modal remains active
void btscan_runtime_close(bool confirmed);
void btscan_runtime_background_step(void);

#endif // BELLATRIX_ENABLE_BTSTACK
