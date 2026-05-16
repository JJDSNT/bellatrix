// src/launcher/launcher.h
// Pre-emulation ADF selector UI.
// Renders on the VC4 framebuffer; input from USB HID keyboard.
// Compiled only when BELLATRIX_LAUNCHER is defined.

#pragma once
#include <stdint.h>
#include <stdbool.h>

// Run the launcher.
// Scans the SD card FAT32 partition for .adf files, shows a selection UI,
// and inserts the chosen file as DF0.
//
// Returns true if a disk was inserted; false if user skipped (ESC) or no
// ADF files were found.
//
// Call after framebuffer is ready and before the main emulation loop.
bool launcher_run(void);
