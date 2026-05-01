// src/core/memory/chip_ram.h

#pragma once

#include <stdint.h>

#include "memory/memory.h"

/* ------------------------------------------------------------------------- */
/* direct Chip RAM access                                                    */
/* ------------------------------------------------------------------------- */

uint8_t  chip_ram_read8 (const BellatrixMemory *m, uint32_t addr);
uint16_t chip_ram_read16(const BellatrixMemory *m, uint32_t addr);
uint32_t chip_ram_read32(const BellatrixMemory *m, uint32_t addr);

void chip_ram_write8 (BellatrixMemory *m, uint32_t addr, uint8_t value);
void chip_ram_write16(BellatrixMemory *m, uint32_t addr, uint16_t value);
void chip_ram_write32(BellatrixMemory *m, uint32_t addr, uint32_t value);

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

uint32_t chip_ram_wrap_addr(const BellatrixMemory *m, uint32_t addr);
int chip_ram_is_configured(const BellatrixMemory *m);