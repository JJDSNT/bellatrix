// src/launcher/launcher.h
// Pre-emulation media selector UI.
// Renders on the VC4 framebuffer; input from USB HID keyboard.
// Compiled only when BELLATRIX_LAUNCHER is defined.

#pragma once
#include <stdint.h>
#include <stdbool.h>

// Run the launcher.
// Scans a USB pen drive for .ISO/.HDF/.ADF files, shows a selection UI, and
// attaches the chosen media as CD-ROM, hard disk and/or DF0-DF3.  Falls back
// to QEMU loader images when no USB drive is present (development use).
//
// Returns true if a disk was inserted; false if the user skipped (ESC)
// or no media was found.
//
// Call after framebuffer is ready and before the main emulation loop.
bool launcher_run(void);

/* Write BTSCAN.TXT to SD with current scan results + bt_diag snapshot.
 * Safe to call more than once — overwrites in place each time.
 * Used by bellatrix.c to capture post-connection-attempt diagnostics. */
void launcher_save_bt_report(void);
