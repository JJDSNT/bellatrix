// src/core/memory/chip_ram.c

#include "memory/chip_ram.h"
#include "support.h"
#include "debug/cpu_pc.h"

/* ------------------------------------------------------------------------- */
/* helpers internos                                                          */
/* ------------------------------------------------------------------------- */

static inline uint32_t chip_addr(const BellatrixMemory *m, uint32_t addr)
{
    return addr & m->chip_ram_mask;
}

static inline int chip_ram_diagrom_trace_addr(uint32_t a)
{
    if ((a >= 0x1f0300u && a <= 0x1f03c0u) ||
        (a >= 0x1f5300u && a <= 0x1f53c0u) ||
        (a >= 0x1fa300u && a <= 0x1fa3c0u) ||
        (a >= 0x1f1300u && a <= 0x1f13c0u) ||
        (a >= 0x1f6300u && a <= 0x1f63c0u) ||
        (a >= 0x1fb300u && a <= 0x1fb3c0u))
    {
        return 1;
    }

    return 0;
}

/* ------------------------------------------------------------------------- */
/* reads                                                                     */
/* ------------------------------------------------------------------------- */

uint8_t chip_ram_read8(const BellatrixMemory *m, uint32_t addr)
{
    return m->chip_ram[chip_addr(m, addr)];
}

uint16_t chip_ram_read16(const BellatrixMemory *m, uint32_t addr)
{
    uint32_t a = chip_addr(m, addr);

    return ((uint16_t)m->chip_ram[a] << 8) |
           ((uint16_t)m->chip_ram[(a + 1) & m->chip_ram_mask]);
}

uint32_t chip_ram_read32(const BellatrixMemory *m, uint32_t addr)
{
    uint32_t a = chip_addr(m, addr);

    return ((uint32_t)m->chip_ram[a] << 24) |
           ((uint32_t)m->chip_ram[(a + 1) & m->chip_ram_mask] << 16) |
           ((uint32_t)m->chip_ram[(a + 2) & m->chip_ram_mask] << 8)  |
           ((uint32_t)m->chip_ram[(a + 3) & m->chip_ram_mask]);
}

/* ------------------------------------------------------------------------- */
/* writes                                                                    */
/* ------------------------------------------------------------------------- */

void chip_ram_write8(BellatrixMemory *m, uint32_t addr, uint8_t value)
{
    uint32_t a = chip_addr(m, addr);

    m->chip_ram[a] = value;

    if (chip_ram_diagrom_trace_addr(a))
    {
        kprintf("[CHIPRAM-W8] pc=%08x addr=%06x val=%02x\n",
                (unsigned)bellatrix_debug_cpu_pc(),
                (unsigned)a,
                (unsigned)value);
    }
}

void chip_ram_write16(BellatrixMemory *m, uint32_t addr, uint16_t value)
{
    uint32_t a = chip_addr(m, addr);

    m->chip_ram[a] = value >> 8;
    m->chip_ram[(a + 1) & m->chip_ram_mask] = value & 0xFF;

    if (chip_ram_diagrom_trace_addr(a))
    {
        kprintf("[CHIPRAM-W16] pc=%08x addr=%06x val=%04x\n",
                (unsigned)bellatrix_debug_cpu_pc(),
                (unsigned)a,
                (unsigned)value);
    }
}

void chip_ram_write32(BellatrixMemory *m, uint32_t addr, uint32_t value)
{
    uint32_t a = chip_addr(m, addr);

    m->chip_ram[a] = value >> 24;
    m->chip_ram[(a + 1) & m->chip_ram_mask] = (value >> 16) & 0xFF;
    m->chip_ram[(a + 2) & m->chip_ram_mask] = (value >> 8) & 0xFF;
    m->chip_ram[(a + 3) & m->chip_ram_mask] = value & 0xFF;

    if (chip_ram_diagrom_trace_addr(a))
    {
        kprintf("[CHIPRAM-W32] pc=%08x addr=%06x val=%08x\n",
                (unsigned)bellatrix_debug_cpu_pc(),
                (unsigned)a,
                (unsigned)value);
    }
}

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

uint32_t chip_ram_wrap_addr(const BellatrixMemory *m, uint32_t addr)
{
    return chip_addr(m, addr);
}

int chip_ram_is_configured(const BellatrixMemory *m)
{
    return (m->chip_ram != 0 && m->chip_ram_size != 0);
}
