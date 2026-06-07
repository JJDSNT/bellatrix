// src/machine/memory/slow_ram.h

#pragma once

#include <stdint.h>

#include "machine/memory/memory.h"

/* ------------------------------------------------------------------------- */
/* direct Slow RAM access                                                    */
/* ------------------------------------------------------------------------- */

uint8_t  slow_ram_read8 (const BellatrixMemory *m, uint32_t addr);
uint16_t slow_ram_read16(const BellatrixMemory *m, uint32_t addr);
uint32_t slow_ram_read32(const BellatrixMemory *m, uint32_t addr);

void slow_ram_write8 (BellatrixMemory *m, uint32_t addr, uint8_t value);
void slow_ram_write16(BellatrixMemory *m, uint32_t addr, uint16_t value);
void slow_ram_write32(BellatrixMemory *m, uint32_t addr, uint32_t value);

int slow_ram_contains(const BellatrixMemory *m, uint32_t addr, unsigned int size);
