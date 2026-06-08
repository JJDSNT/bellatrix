// src/launcher/launcher.h
// Pre-emulation ADF selector UI.
// Renders on the VC4 framebuffer; input from USB HID keyboard.
// Compiled only when BELLATRIX_LAUNCHER is defined.

#pragma once
#include <stdint.h>
#include <stdbool.h>

// Run the launcher.
// Scans a USB pen drive for .ADF/.ISO files, shows a selection UI, and
// inserts the chosen file as DF0 or CD-ROM.  Falls back to QEMU loader
// images when no USB drive is present (development use).
//
// Returns true if a disk was inserted; false if the user skipped (ESC)
// or no media was found.
//
// Call after framebuffer is ready and before the main emulation loop.
bool launcher_run(void);
