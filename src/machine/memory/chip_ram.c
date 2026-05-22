// src/machine/memory/chip_ram.c

#include "machine/memory/chip_ram.h"
#include "support.h"

/* ------------------------------------------------------------------------- */
/* helpers internos                                                          */
/* ------------------------------------------------------------------------- */

static inline uint32_t chip_addr(const BellatrixMemory *m, uint32_t addr)
{
    return addr & m->chip_ram_mask;
}

/* ------------------------------------------------------------------------- */
/* reads                                                                     */
/* ------------------------------------------------------------------------- */

uint8_t chip_ram_read8(const BellatrixMemory *m, uint32_t addr)
{
    uint32_t a = chip_addr(m, addr);
    return m->chip_ram[a];
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
}

void chip_ram_write16(BellatrixMemory *m, uint32_t addr, uint16_t value)
{
    uint32_t a = chip_addr(m, addr);
    m->chip_ram[a] = value >> 8;
    m->chip_ram[(a + 1) & m->chip_ram_mask] = value & 0xFF;
}

void chip_ram_write32(BellatrixMemory *m, uint32_t addr, uint32_t value)
{
    uint32_t a = chip_addr(m, addr);
    m->chip_ram[a] = value >> 24;
    m->chip_ram[(a + 1) & m->chip_ram_mask] = (value >> 16) & 0xFF;
    m->chip_ram[(a + 2) & m->chip_ram_mask] = (value >> 8) & 0xFF;
    m->chip_ram[(a + 3) & m->chip_ram_mask] = value & 0xFF;
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
