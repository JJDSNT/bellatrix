// src/core/memory/memory_map.h

#pragma once

#include <stdint.h>
#include "memory/memory.h"

/* ------------------------------------------------------------------------- */
/* memory regions                                                            */
/* ------------------------------------------------------------------------- */

typedef enum
{
    MEM_REGION_CHIP_RAM = 0,
    MEM_REGION_ROM,
    MEM_REGION_EXP_ROM_CHECK,
    MEM_REGION_CUSTOM,
    MEM_REGION_CIAA,
    MEM_REGION_CIAB,
    MEM_REGION_FAST,
    MEM_REGION_Z2,
    MEM_REGION_Z3,
    MEM_REGION_UNKNOWN

} MemoryRegion;

/* ------------------------------------------------------------------------- */
/* decode                                                                    */
/* ------------------------------------------------------------------------- */

MemoryRegion memory_map_decode(uint32_t addr);

/* ------------------------------------------------------------------------- */
/* generic access                                                            */
/* ------------------------------------------------------------------------- */

uint8_t  memory_map_read8 (BellatrixMemory *m, uint32_t addr);
uint16_t memory_map_read16(BellatrixMemory *m, uint32_t addr);
uint32_t memory_map_read32(BellatrixMemory *m, uint32_t addr);

void memory_map_write8 (BellatrixMemory *m, uint32_t addr, uint8_t value);
void memory_map_write16(BellatrixMemory *m, uint32_t addr, uint16_t value);
void memory_map_write32(BellatrixMemory *m, uint32_t addr, uint32_t value);
