// src/launcher/media_selection.h
// Media (ISO/HDF/ADF) enumeration, selection and attach for the launcher.
//
// Scans the USB pen drive (or QEMU loader memory) for bootable media, shows
// the selection UI, and attaches the chosen media as CD-ROM, hard disk and/or
// DF0-DF3. Assumes launcher input is already active; releases it on exit.
#pragma once
#include <stdbool.h>

// Returns true if a disk was inserted; false if the user skipped (ESC) or no
// media was found.
bool media_selection_run(void);

// True when the QEMU generic loader injected a recognizable ADF or ISO.
// This lets the boot coordinator remain visually silent when neither USB
// media nor development-loader media is present.
bool media_selection_qemu_media_present(void);

// Runtime modal lifecycle. Runtime selection is intentionally ADF-only until
// HDF/ISO hot-swap ownership has an equally explicit safe boundary.
bool media_selection_runtime_open(void);
bool media_selection_runtime_step(void); // true while modal remains active
void media_selection_runtime_close(void);
