// src/core/memory/memory.h

#pragma once

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------------- */
/* memory constants                                                          */
/* ------------------------------------------------------------------------- */

#define BELLATRIX_CHIP_RAM_BASE 0x00000000u
#define BELLATRIX_CHIP_RAM_SIZE 0x00200000u
#define BELLATRIX_CHIP_RAM_MASK 0x001FFFFFu

/*
 * Phase 1 AROS boot target:
 * static Zorro II-style Fast RAM.
 */
#define BELLATRIX_FAST_RAM_BASE 0x00200000u
#define BELLATRIX_FAST_RAM_SIZE 0x00800000u
#define BELLATRIX_FAST_RAM_END  0x009FFFFFu
#define BELLATRIX_FAST_RAM_MASK 0x007FFFFFu

#define BELLATRIX_ROM_BASE      0x00F80000u
#define BELLATRIX_ROM_SIZE      0x00080000u
#define BELLATRIX_ROM_END       0x00FFFFFFu

#define BELLATRIX_CUSTOM_BASE   0x00DFF000u
#define BELLATRIX_CUSTOM_END    0x00DFFFFFu

#define BELLATRIX_CIAB_BASE     0x00BFD000u
#define BELLATRIX_CIAB_END      0x00BFDFFFu

#define BELLATRIX_CIAA_BASE     0x00BFE000u
#define BELLATRIX_CIAA_END      0x00BFEFFFu

#define BELLATRIX_Z2_CONFIG_BASE 0x00E80000u
#define BELLATRIX_Z2_CONFIG_END  0x00EFFFFFu

#define BELLATRIX_Z3_BASE        0x10000000u

/* ------------------------------------------------------------------------- */
/* memory backing                                                            */
/* ------------------------------------------------------------------------- */

typedef struct BellatrixMemory
{
    uint8_t *chip_ram;
    size_t   chip_ram_size;
    uint32_t chip_ram_mask;

    uint8_t *fast_ram;
    size_t   fast_ram_size;
    uint32_t fast_ram_mask;

    const uint8_t *rom;
    size_t         rom_size;

    uint8_t overlay_enabled;

} BellatrixMemory;

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void bellatrix_memory_init(BellatrixMemory *mem);

void bellatrix_memory_reset(BellatrixMemory *mem);

void bellatrix_memory_attach_rom(BellatrixMemory *mem,
                                 const uint8_t *rom,
                                 size_t rom_size);

/* ------------------------------------------------------------------------- */
/* overlay                                                                   */
/* ------------------------------------------------------------------------- */

void bellatrix_memory_set_overlay(BellatrixMemory *mem, int enabled);
int  bellatrix_memory_overlay_enabled(const BellatrixMemory *mem);

/* ------------------------------------------------------------------------- */
/* generic machine memory API                                                */
/* ------------------------------------------------------------------------- */

uint8_t  bellatrix_mem_read8 (BellatrixMemory *mem, uint32_t addr);
uint16_t bellatrix_mem_read16(BellatrixMemory *mem, uint32_t addr);
uint32_t bellatrix_mem_read32(BellatrixMemory *mem, uint32_t addr);

void bellatrix_mem_write8 (BellatrixMemory *mem, uint32_t addr, uint8_t value);
void bellatrix_mem_write16(BellatrixMemory *mem, uint32_t addr, uint16_t value);
void bellatrix_mem_write32(BellatrixMemory *mem, uint32_t addr, uint32_t value);

/* ------------------------------------------------------------------------- */
/* direct Chip RAM API                                                       */
/* ------------------------------------------------------------------------- */

uint8_t  bellatrix_chip_read8 (const BellatrixMemory *mem, uint32_t addr);
uint16_t bellatrix_chip_read16(const BellatrixMemory *mem, uint32_t addr);
uint32_t bellatrix_chip_read32(const BellatrixMemory *mem, uint32_t addr);

void bellatrix_chip_write8 (BellatrixMemory *mem, uint32_t addr, uint8_t value);
void bellatrix_chip_write16(BellatrixMemory *mem, uint32_t addr, uint16_t value);
void bellatrix_chip_write32(BellatrixMemory *mem, uint32_t addr, uint32_t value);

/* ------------------------------------------------------------------------- */
/* direct Fast RAM API                                                       */
/* ------------------------------------------------------------------------- */

uint8_t  bellatrix_fast_read8 (const BellatrixMemory *mem, uint32_t addr);
uint16_t bellatrix_fast_read16(const BellatrixMemory *mem, uint32_t addr);
uint32_t bellatrix_fast_read32(const BellatrixMemory *mem, uint32_t addr);

void bellatrix_fast_write8 (BellatrixMemory *mem, uint32_t addr, uint8_t value);
void bellatrix_fast_write16(BellatrixMemory *mem, uint32_t addr, uint16_t value);
void bellatrix_fast_write32(BellatrixMemory *mem, uint32_t addr, uint32_t value);

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

uint32_t bellatrix_chip_wrap_addr(const BellatrixMemory *mem, uint32_t addr);
int      bellatrix_chip_is_configured(const BellatrixMemory *mem);