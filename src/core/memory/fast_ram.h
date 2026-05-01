// src/core/memory/fast_ram.h

#pragma once

#include <stdint.h>

#include "memory/memory.h"

/* ------------------------------------------------------------------------- */
/* direct Fast RAM access                                                    */
/* ------------------------------------------------------------------------- */

uint8_t  fast_ram_read8 (const BellatrixMemory *m, uint32_t addr);
uint16_t fast_ram_read16(const BellatrixMemory *m, uint32_t addr);
uint32_t fast_ram_read32(const BellatrixMemory *m, uint32_t addr);

void fast_ram_write8 (BellatrixMemory *m, uint32_t addr, uint8_t value);
void fast_ram_write16(BellatrixMemory *m, uint32_t addr, uint16_t value);
void fast_ram_write32(BellatrixMemory *m, uint32_t addr, uint32_t value);