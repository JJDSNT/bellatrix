// src/core/memory/overlay.h

#pragma once

#include <stdint.h>
#include "memory/memory.h"

/*
 * Overlay control:
 *
 * enabled = 1 → ROM visible at 0x000000 (reads)
 * enabled = 0 → Chip RAM visible at 0x000000
 *
 * Writes ALWAYS go to Chip RAM (Amiga rule)
 */

void overlay_set(BellatrixMemory *m, int enabled);
int  overlay_enabled(const BellatrixMemory *m);

/*
 * Read helpers that apply overlay logic.
 * Used by generic memory path (CPU side).
 */

uint8_t  overlay_read8 (const BellatrixMemory *m, uint32_t addr);
uint16_t overlay_read16(const BellatrixMemory *m, uint32_t addr);
uint32_t overlay_read32(const BellatrixMemory *m, uint32_t addr);