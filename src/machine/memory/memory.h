// src/machine/memory/memory.h

#pragma once

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------------- */
/* memory constants                                                          */
/* ------------------------------------------------------------------------- */

#define BELLATRIX_CHIP_RAM_BASE 0x00000000u
#define BELLATRIX_CHIP_RAM_SIZE 0x00100000u
#define BELLATRIX_CHIP_RAM_END  (BELLATRIX_CHIP_RAM_BASE + BELLATRIX_CHIP_RAM_SIZE - 1u)
#define BELLATRIX_CHIP_RAM_MASK 0x000FFFFFu

/*
 * Phase 1 AROS boot target:
 * static Zorro II-style Fast RAM.
 */
#define BELLATRIX_FAST_RAM_BASE 0x00200000u
#define BELLATRIX_FAST_RAM_SIZE 0x00800000u
#define BELLATRIX_FAST_RAM_END  0x009FFFFFu
#define BELLATRIX_FAST_RAM_MASK 0x007FFFFFu

/*
 * AROS-visible motherboard slow RAM.  AROS __MemoryTest discovers this range
 * and gives it higher priority than chip RAM, keeping chip RAM free for DMA
 * buffers and display data.
 */
#define BELLATRIX_SLOW_RAM_BASE 0x00C00000u
#define BELLATRIX_SLOW_RAM_SIZE 0x00180000u
#define BELLATRIX_SLOW_RAM_END  (BELLATRIX_SLOW_RAM_BASE + BELLATRIX_SLOW_RAM_SIZE - 1u)

#define BELLATRIX_ROM_BASE      0x00F80000u
#define BELLATRIX_ROM_SIZE      0x00080000u
#define BELLATRIX_ROM_END       0x00FFFFFFu

/* Extended ROM window — first 512 KB of a 1 MB ROM (e.g., AROS modules). */
#define BELLATRIX_EXT_ROM_BASE  0x00E00000u
#define BELLATRIX_EXT_ROM_SIZE  0x00080000u
#define BELLATRIX_EXT_ROM_END   0x00E7FFFFu

#define BELLATRIX_CHIP_BOOT_SIZE BELLATRIX_ROM_SIZE
#define BELLATRIX_CHIP_BOOT_END  (BELLATRIX_CHIP_RAM_BASE + BELLATRIX_CHIP_BOOT_SIZE - 1u)

#define BELLATRIX_CUSTOM_BASE   0x00DFF000u
#define BELLATRIX_CUSTOM_END    0x00DFFFFFu

#define BELLATRIX_CIAB_BASE     0x00BFD000u
#define BELLATRIX_CIAB_END      0x00BFDFFFu

#define BELLATRIX_CIAA_BASE     0x00BFE000u
#define BELLATRIX_CIAA_END      0x00BFEFFFu

#define BELLATRIX_Z2_CONFIG_BASE 0x00E80000u
#define BELLATRIX_Z2_CONFIG_END  0x00EFFFFFu

/* ------------------------------------------------------------------------- */
/* address helpers                                                           */
/* ------------------------------------------------------------------------- */

static inline int bellatrix_chip_addr_contains(uint32_t addr)
{
    return addr < BELLATRIX_CHIP_RAM_SIZE;
}

static inline int bellatrix_chip_addr_contains_range(uint32_t addr, uint32_t size)
{
    return addr < BELLATRIX_CHIP_RAM_SIZE &&
           size <= (BELLATRIX_CHIP_RAM_SIZE - addr);
}

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

    uint8_t *slow_ram;
    size_t   slow_ram_size;
    uint8_t  slow_ram_enabled;

    const uint8_t *rom;
    size_t         rom_size;

    /* Extended ROM window (0xe00000-0xe7ffff) — first half of 1 MB ROMs only. */
    const uint8_t *rom_ext;
    size_t         rom_ext_size;

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

void bellatrix_memory_attach_ext_rom(BellatrixMemory *mem,
                                     const uint8_t *rom_ext,
                                     size_t rom_ext_size);

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
/* direct Slow RAM API                                                       */
/* ------------------------------------------------------------------------- */

uint8_t  bellatrix_slow_read8 (const BellatrixMemory *mem, uint32_t addr);
uint16_t bellatrix_slow_read16(const BellatrixMemory *mem, uint32_t addr);
uint32_t bellatrix_slow_read32(const BellatrixMemory *mem, uint32_t addr);

void bellatrix_slow_write8 (BellatrixMemory *mem, uint32_t addr, uint8_t value);
void bellatrix_slow_write16(BellatrixMemory *mem, uint32_t addr, uint16_t value);
void bellatrix_slow_write32(BellatrixMemory *mem, uint32_t addr, uint32_t value);

int bellatrix_slow_contains(const BellatrixMemory *mem, uint32_t addr, unsigned int size);

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

uint32_t bellatrix_chip_wrap_addr(const BellatrixMemory *mem, uint32_t addr);
int      bellatrix_chip_is_configured(const BellatrixMemory *mem);
