// src/machine/memory/memory_map.c

#include "machine/memory/memory_map.h"

#include "machine/memory/chip_ram.h"
#include "machine/memory/overlay.h"
#include "machine/memory/slow_ram.h"

/* Zorro II/III AutoConfig and board windows are decoded and served upstream by
 * machine_rigel_bus.c (board_registry + expansion.c bus_ops); memory_map only
 * sees the addresses those layers don't claim, so it has no Z2 routing. */

/* ------------------------------------------------------------------------- */
/* decode                                                                    */
/* ------------------------------------------------------------------------- */

MemoryRegion memory_map_decode(uint32_t addr)
{
    /* 68000/68010: 24-bit address bus — upper byte is not driven by the CPU.
     * Mask here so 32-bit-wrapped LVO addresses (e.g. exec_base - 516 when
     * exec_base is 0x30) resolve to their correct 24-bit ROM aliases. */
    addr &= 0x00FFFFFFu;

    if (bellatrix_chip_cpu_addr_contains(addr))
        return MEM_REGION_CHIP_RAM;

    if (addr >= BELLATRIX_SLOW_RAM_BASE && addr <= BELLATRIX_SLOW_RAM_END)
        return MEM_REGION_SLOW;

    if (bellatrix_ciab_addr_contains(addr))
        return MEM_REGION_CIAB;

    if (bellatrix_ciaa_addr_contains(addr))
        return MEM_REGION_CIAA;

    if (bellatrix_custom_addr_contains(addr))
        return MEM_REGION_CUSTOM;

    if (addr >= 0x00F00000u && addr <= 0x00F7FFFFu)
        return MEM_REGION_EXP_ROM_CHECK;

    if (addr >= BELLATRIX_ROM_BASE && addr <= BELLATRIX_ROM_END)
        return MEM_REGION_ROM;

#if !defined(BELLATRIX_EMU68)
    if (addr >= 0x10000000u)
        return MEM_REGION_Z3;
#endif

    return MEM_REGION_UNKNOWN;
}

/* ------------------------------------------------------------------------- */
/* read                                                                      */
/* ------------------------------------------------------------------------- */

uint8_t memory_map_read8(BellatrixMemory *m, uint32_t addr)
{
    addr &= 0x00FFFFFFu;
    switch (memory_map_decode(addr))
    {
    case MEM_REGION_CHIP_RAM:
        return overlay_read8(m, addr);

    case MEM_REGION_ROM:
        return overlay_read8(m, addr);

    case MEM_REGION_EXP_ROM_CHECK:
        return 0x00u;

    case MEM_REGION_SLOW:
        return slow_ram_read8(m, addr);


    default:
        return 0xFFu;
    }
}

uint16_t memory_map_read16(BellatrixMemory *m, uint32_t addr)
{
    addr &= 0x00FFFFFFu;
    switch (memory_map_decode(addr))
    {
    case MEM_REGION_CHIP_RAM:
        return overlay_read16(m, addr);

    case MEM_REGION_ROM:
        return overlay_read16(m, addr);

    case MEM_REGION_EXP_ROM_CHECK:
        return 0x0000u;

    case MEM_REGION_SLOW:
        return slow_ram_read16(m, addr);


    default:
        return 0xFFFFu;
    }
}

uint32_t memory_map_read32(BellatrixMemory *m, uint32_t addr)
{
    addr &= 0x00FFFFFFu;
    switch (memory_map_decode(addr))
    {
    case MEM_REGION_CHIP_RAM:
        return overlay_read32(m, addr);

    case MEM_REGION_ROM:
        return overlay_read32(m, addr);

    case MEM_REGION_EXP_ROM_CHECK:
        return 0x00000000u;

    case MEM_REGION_SLOW:
        return slow_ram_read32(m, addr);


    default:
        return 0xFFFFFFFFu;
    }
}

/* ------------------------------------------------------------------------- */
/* write                                                                     */
/* ------------------------------------------------------------------------- */

void memory_map_write8(BellatrixMemory *m, uint32_t addr, uint8_t value)
{
    addr &= 0x00FFFFFFu;
    switch (memory_map_decode(addr))
    {
    case MEM_REGION_CHIP_RAM:
        chip_ram_write8(m, addr, value);
        return;

    case MEM_REGION_SLOW:
        slow_ram_write8(m, addr, value);
        return;


    default:
        return;
    }
}

void memory_map_write16(BellatrixMemory *m, uint32_t addr, uint16_t value)
{
    addr &= 0x00FFFFFFu;
    switch (memory_map_decode(addr))
    {
    case MEM_REGION_CHIP_RAM:
        chip_ram_write16(m, addr, value);
        return;

    case MEM_REGION_SLOW:
        slow_ram_write16(m, addr, value);
        return;


    default:
        return;
    }
}

void memory_map_write32(BellatrixMemory *m, uint32_t addr, uint32_t value)
{
    addr &= 0x00FFFFFFu;
    switch (memory_map_decode(addr))
    {
    case MEM_REGION_CHIP_RAM:
        chip_ram_write32(m, addr, value);
        return;

    case MEM_REGION_SLOW:
        slow_ram_write32(m, addr, value);
        return;


    default:
        return;
    }
}
