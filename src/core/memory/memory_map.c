// src/core/memory/memory_map.c

#include "memory/memory_map.h"

#include "memory/chip_ram.h"
#include "memory/overlay.h"

/* autoconfig and fast RAM are needed:
 *   - always on the Musashi harness (all accesses go through the bus handler)
 *   - on Emu68 legacy mode (boards disabled) for Z2 config accesses only;
 *     fast RAM itself is identity-mapped via MMU and never reaches the bus */
#if defined(BELLATRIX_HARNESS)
#include "memory/fast_ram.h"
#endif

#if defined(BELLATRIX_HARNESS) || \
    (defined(BELLATRIX_ENABLE_EMU68_BOARDS) && !BELLATRIX_ENABLE_EMU68_BOARDS)
#include "bus/zorro2/zorro2_bus.h"
#define BELLATRIX_ROUTE_Z2_AUTOCONFIG 1
#else
#define BELLATRIX_ROUTE_Z2_AUTOCONFIG 0
#endif

/* ------------------------------------------------------------------------- */
/* decode                                                                    */
/* ------------------------------------------------------------------------- */

MemoryRegion memory_map_decode(uint32_t addr)
{
    /* 68000/68010: 24-bit address bus — upper byte is not driven by the CPU.
     * Mask here so 32-bit-wrapped LVO addresses (e.g. exec_base - 516 when
     * exec_base is 0x30) resolve to their correct 24-bit ROM aliases. */
    addr &= 0x00FFFFFFu;

    if (bellatrix_chip_addr_contains(addr))
        return MEM_REGION_CHIP_RAM;

#if defined(BELLATRIX_HARNESS)
    if (addr >= BELLATRIX_FAST_RAM_BASE && addr <= BELLATRIX_FAST_RAM_END)
        return MEM_REGION_FAST;
#endif

    if (addr >= BELLATRIX_CIAB_BASE && addr <= BELLATRIX_CIAB_END)
        return MEM_REGION_CIAB;

    if (addr >= BELLATRIX_CIAA_BASE && addr <= BELLATRIX_CIAA_END)
        return MEM_REGION_CIAA;

    if (addr >= BELLATRIX_CUSTOM_BASE && addr <= BELLATRIX_CUSTOM_END)
        return MEM_REGION_CUSTOM;

#if BELLATRIX_ROUTE_Z2_AUTOCONFIG
    if (bellatrix_zorro2_board_window_contains(addr))
        return MEM_REGION_Z2_BOARD;

    if (bellatrix_zorro2_config_window_contains(addr))
        return MEM_REGION_Z2;
#endif

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

#if defined(BELLATRIX_HARNESS)
    case MEM_REGION_FAST:
        return fast_ram_read8(m, addr);
#endif

#if BELLATRIX_ROUTE_Z2_AUTOCONFIG
    case MEM_REGION_Z2:
        return bellatrix_zorro2_config_read8(m, addr);

    case MEM_REGION_Z2_BOARD:
        return bellatrix_zorro2_board_read8(addr);
#endif

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

#if defined(BELLATRIX_HARNESS)
    case MEM_REGION_FAST:
        return fast_ram_read16(m, addr);
#endif

#if BELLATRIX_ROUTE_Z2_AUTOCONFIG
    case MEM_REGION_Z2:
        return bellatrix_zorro2_config_read16(m, addr);

    case MEM_REGION_Z2_BOARD:
        return bellatrix_zorro2_board_read16(addr);
#endif

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

#if defined(BELLATRIX_HARNESS)
    case MEM_REGION_FAST:
        return fast_ram_read32(m, addr);
#endif

#if BELLATRIX_ROUTE_Z2_AUTOCONFIG
    case MEM_REGION_Z2:
        return bellatrix_zorro2_config_read32(m, addr);

    case MEM_REGION_Z2_BOARD:
        return bellatrix_zorro2_board_read32(addr);
#endif

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

#if defined(BELLATRIX_HARNESS)
    case MEM_REGION_FAST:
        fast_ram_write8(m, addr, value);
        return;
#endif

#if BELLATRIX_ROUTE_Z2_AUTOCONFIG
    case MEM_REGION_Z2:
        bellatrix_zorro2_config_write8(m, addr, value);
        return;
#endif

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

#if defined(BELLATRIX_HARNESS)
    case MEM_REGION_FAST:
        fast_ram_write16(m, addr, value);
        return;
#endif

#if BELLATRIX_ROUTE_Z2_AUTOCONFIG
    case MEM_REGION_Z2:
        bellatrix_zorro2_config_write16(m, addr, value);
        return;
#endif

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

#if defined(BELLATRIX_HARNESS)
    case MEM_REGION_FAST:
        fast_ram_write32(m, addr, value);
        return;
#endif

#if BELLATRIX_ROUTE_Z2_AUTOCONFIG
    case MEM_REGION_Z2:
        bellatrix_zorro2_config_write32(m, addr, value);
        return;
#endif

    default:
        return;
    }
}
