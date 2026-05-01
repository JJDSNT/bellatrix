// src/core/memory/memory_map.c

#include "memory/memory_map.h"

#include "memory/autoconfig.h"
#include "memory/chip_ram.h"
#include "memory/fast_ram.h"
#include "memory/overlay.h"

/* ------------------------------------------------------------------------- */
/* decode                                                                    */
/* ------------------------------------------------------------------------- */

MemoryRegion memory_map_decode(uint32_t addr)
{
    if (addr < 0x00200000u)
        return MEM_REGION_CHIP_RAM;

    if (addr >= 0x00200000u && addr <= 0x009FFFFFu)
        return MEM_REGION_FAST;

    if (addr >= 0x00BFD000u && addr <= 0x00BFDFFFu)
        return MEM_REGION_CIAB;

    if (addr >= 0x00BFE000u && addr <= 0x00BFEFFFu)
        return MEM_REGION_CIAA;

    if (addr >= 0x00DFF000u && addr <= 0x00DFFFFFu)
        return MEM_REGION_CUSTOM;

    if (addr >= 0x00E80000u && addr <= 0x00EFFFFFu)
        return MEM_REGION_Z2;

    if (addr >= 0x00F00000u && addr <= 0x00F7FFFFu)
        return MEM_REGION_EXP_ROM_CHECK;

    if (addr >= 0x00F80000u && addr <= 0x00FFFFFFu)
        return MEM_REGION_ROM;

    if (addr >= 0x10000000u)
        return MEM_REGION_Z3;

    return MEM_REGION_UNKNOWN;
}

/* ------------------------------------------------------------------------- */
/* read                                                                      */
/* ------------------------------------------------------------------------- */

uint8_t memory_map_read8(BellatrixMemory *m, uint32_t addr)
{
    switch (memory_map_decode(addr))
    {
    case MEM_REGION_CHIP_RAM:
        return overlay_read8(m, addr);

    case MEM_REGION_ROM:
        return overlay_read8(m, addr);

    case MEM_REGION_EXP_ROM_CHECK:
        return 0x00u;

    case MEM_REGION_FAST:
        return fast_ram_read8(m, addr);

    case MEM_REGION_Z2:
        return autoconfig_read8(m, addr);

    default:
        return 0xFFu;
    }
}

uint16_t memory_map_read16(BellatrixMemory *m, uint32_t addr)
{
    switch (memory_map_decode(addr))
    {
    case MEM_REGION_CHIP_RAM:
        return overlay_read16(m, addr);

    case MEM_REGION_ROM:
        return overlay_read16(m, addr);

    case MEM_REGION_EXP_ROM_CHECK:
        return 0x0000u;

    case MEM_REGION_FAST:
        return fast_ram_read16(m, addr);

    case MEM_REGION_Z2:
        return autoconfig_read16(m, addr);

    default:
        return 0xFFFFu;
    }
}

uint32_t memory_map_read32(BellatrixMemory *m, uint32_t addr)
{
    switch (memory_map_decode(addr))
    {
    case MEM_REGION_CHIP_RAM:
        return overlay_read32(m, addr);

    case MEM_REGION_ROM:
        return overlay_read32(m, addr);

    case MEM_REGION_EXP_ROM_CHECK:
        return 0x00000000u;

    case MEM_REGION_FAST:
        return fast_ram_read32(m, addr);

    case MEM_REGION_Z2:
        return autoconfig_read32(m, addr);

    default:
        return 0xFFFFFFFFu;
    }
}

/* ------------------------------------------------------------------------- */
/* write                                                                     */
/* ------------------------------------------------------------------------- */

void memory_map_write8(BellatrixMemory *m, uint32_t addr, uint8_t value)
{
    switch (memory_map_decode(addr))
    {
    case MEM_REGION_CHIP_RAM:
        chip_ram_write8(m, addr, value);
        return;

    case MEM_REGION_FAST:
        fast_ram_write8(m, addr, value);
        return;

    case MEM_REGION_Z2:
        autoconfig_write8(m, addr, value);
        return;

    default:
        return;
    }
}

void memory_map_write16(BellatrixMemory *m, uint32_t addr, uint16_t value)
{
    switch (memory_map_decode(addr))
    {
    case MEM_REGION_CHIP_RAM:
        chip_ram_write16(m, addr, value);
        return;

    case MEM_REGION_FAST:
        fast_ram_write16(m, addr, value);
        return;

    case MEM_REGION_Z2:
        autoconfig_write16(m, addr, value);
        return;

    default:
        return;
    }
}

void memory_map_write32(BellatrixMemory *m, uint32_t addr, uint32_t value)
{
    switch (memory_map_decode(addr))
    {
    case MEM_REGION_CHIP_RAM:
        chip_ram_write32(m, addr, value);
        return;

    case MEM_REGION_FAST:
        fast_ram_write32(m, addr, value);
        return;

    case MEM_REGION_Z2:
        autoconfig_write32(m, addr, value);
        return;

    default:
        return;
    }
}
