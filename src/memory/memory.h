#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct BellatrixMemory
{
    uint8_t  *chip_ram;
    uint32_t  chip_ram_size;
    uint32_t  chip_ram_mask;
} BellatrixMemory;

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void bellatrix_memory_init(BellatrixMemory *m);
void bellatrix_memory_reset(BellatrixMemory *m);

/* ------------------------------------------------------------------------- */
/* chip RAM access                                                           */
/* ------------------------------------------------------------------------- */

uint8_t  bellatrix_chip_read8 (const BellatrixMemory *m, uint32_t addr);
uint16_t bellatrix_chip_read16(const BellatrixMemory *m, uint32_t addr);
uint32_t bellatrix_chip_read32(const BellatrixMemory *m, uint32_t addr);

void bellatrix_chip_write8 (BellatrixMemory *m, uint32_t addr, uint8_t  value);
void bellatrix_chip_write16(BellatrixMemory *m, uint32_t addr, uint16_t value);
void bellatrix_chip_write32(BellatrixMemory *m, uint32_t addr, uint32_t value);

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

uint32_t bellatrix_chip_wrap_addr(const BellatrixMemory *m, uint32_t addr);
int      bellatrix_chip_is_configured(const BellatrixMemory *m);