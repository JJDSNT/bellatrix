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
