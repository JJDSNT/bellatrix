// src/core/memory/fast_ram.c

#include "memory/fast_ram.h"

/* ------------------------------------------------------------------------- */
/* helpers internos                                                          */
/* ------------------------------------------------------------------------- */

static inline uint32_t fast_addr(const BellatrixMemory *m, uint32_t addr)
{
    return addr & m->fast_ram_mask;
}

/* ------------------------------------------------------------------------- */
/* reads                                                                     */
/* ------------------------------------------------------------------------- */

uint8_t fast_ram_read8(const BellatrixMemory *m, uint32_t addr)
{
    return m->fast_ram[fast_addr(m, addr)];
}

uint16_t fast_ram_read16(const BellatrixMemory *m, uint32_t addr)
{
    uint32_t a = fast_addr(m, addr);

    return ((uint16_t)m->fast_ram[a] << 8) |
           ((uint16_t)m->fast_ram[(a + 1) & m->fast_ram_mask]);
}

uint32_t fast_ram_read32(const BellatrixMemory *m, uint32_t addr)
{
    uint32_t a = fast_addr(m, addr);

    return ((uint32_t)m->fast_ram[a] << 24) |
           ((uint32_t)m->fast_ram[(a + 1) & m->fast_ram_mask] << 16) |
           ((uint32_t)m->fast_ram[(a + 2) & m->fast_ram_mask] << 8)  |
           ((uint32_t)m->fast_ram[(a + 3) & m->fast_ram_mask]);
}

/* ------------------------------------------------------------------------- */
/* writes                                                                    */
/* ------------------------------------------------------------------------- */

void fast_ram_write8(BellatrixMemory *m, uint32_t addr, uint8_t value)
{
    m->fast_ram[fast_addr(m, addr)] = value;
}

void fast_ram_write16(BellatrixMemory *m, uint32_t addr, uint16_t value)
{
    uint32_t a = fast_addr(m, addr);

    m->fast_ram[a] = value >> 8;
    m->fast_ram[(a + 1) & m->fast_ram_mask] = value & 0xFF;
}

void fast_ram_write32(BellatrixMemory *m, uint32_t addr, uint32_t value)
{
    uint32_t a = fast_addr(m, addr);

    m->fast_ram[a] = value >> 24;
    m->fast_ram[(a + 1) & m->fast_ram_mask] = (value >> 16) & 0xFF;
    m->fast_ram[(a + 2) & m->fast_ram_mask] = (value >> 8) & 0xFF;
    m->fast_ram[(a + 3) & m->fast_ram_mask] = value & 0xFF;
}
