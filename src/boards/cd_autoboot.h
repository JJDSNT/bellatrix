// src/boards/cd_autoboot.h
//
// Host-side: registers the CD automount ROM as a Zorro II Autoconfig board.
//
// At boot AROS's expansion library probes 0xE80000, reads the board's
// er_Type (0xD1 = ERT_ZORROII|ERTF_DIAGVALID|64KB) and er_InitDiagVec
// (0x0010), then copies da_Size (0x8000) bytes from board_base+0x0010 to
// an AllocMem'd buffer and calls da_DiagPoint.  The entry point creates a
// low-priority background task that waits for dos.library and then calls
// MountDrive("ata.device", 0, cdBoot=TRUE).
//
// Because AROS CDFS's first AddBootNode fires before ata.device's daemon
// increments au_ChangeNum, the second AddBootNode (issued by MountDrive
// ~3 s later) sees au_ChangeNum=1 and BCache_Present succeeds → ISO mounts.

#pragma once

#include "core/machine.h"

// Register the CD automount ROM with the autoconfig subsystem.
// rom_path: path to the compiled m68k binary (cdmount.rom).
//           Build it with: make -C src/boards/resident
// Returns 0 on success, -1 if file not found (automount silently disabled).
int  cd_autoboot_init(BellatrixMachine *m, const char *rom_path);

// Release the ROM buffer allocated by cd_autoboot_init().
void cd_autoboot_free(void);
