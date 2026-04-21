// src/variants/bellatrix/bellatrix.h
//
// Public interface between Emu68's fault handler and the Bellatrix
// chipset emulator. This is the only header included by Emu68 code
// (via the 0002 patch to vectors.c).

#ifndef _BELLATRIX_H
#define _BELLATRIX_H

#include <stdint.h>

#define BUS_READ  0
#define BUS_WRITE 1

// Called once at startup before M68K_StartEmu().
void bellatrix_init(void);

// Reset vectors captured by bellatrix_init() directly from the ROM ARM
// address — stable before any Emu68 JIT/cache init runs.
// Both are M68K-side values (big-endian already decoded to host uint32_t).
extern uint32_t bellatrix_reset_isp;
extern uint32_t bellatrix_reset_pc;

// Called from SYSWriteValToAddr / SYSReadValFromAddr for every
// access to an unmapped Amiga address (chipset, CIA, slow RAM, etc.).
//
//   addr  — 24-bit Amiga bus address (already normalised by Emu68)
//   value — data to write (BUS_WRITE) or 0 (BUS_READ)
//   size  — access width in bytes: 1, 2, 4, 8, or 16
//   dir   — BUS_READ or BUS_WRITE
//
// Returns the value to load into the destination register (BUS_READ),
// or 0 (BUS_WRITE).
uint32_t bellatrix_bus_access(uint32_t addr, uint32_t value, int size, int dir);

#endif /* _BELLATRIX_H */
