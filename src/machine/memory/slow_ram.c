// src/machine/memory/slow_ram.c

#include "machine/memory/slow_ram.h"

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

static int slow_offset(const BellatrixMemory *m,
                       uint32_t addr,
                       unsigned int size,
                       uint32_t *off)
{
    uint32_t raw = addr & 0x00FFFFFFu;

    if (!m || !m->slow_ram_enabled || !m->slow_ram || !m->slow_ram_size)
        return 0;
    if (size != 1u && size != 2u && size != 4u)
        return 0;
    if (raw < BELLATRIX_SLOW_RAM_BASE || raw >= BELLATRIX_SLOW_RAM_BASE + m->slow_ram_size)
        return 0;

    raw -= BELLATRIX_SLOW_RAM_BASE;
    if (raw > m->slow_ram_size - size)
        return 0;

    *off = raw;
    return 1;
}

int slow_ram_contains(const BellatrixMemory *m, uint32_t addr, unsigned int size)
{
    uint32_t off;

    return slow_offset(m, addr, size, &off);
}

/* ------------------------------------------------------------------------- */
/* reads                                                                     */
/* ------------------------------------------------------------------------- */

uint8_t slow_ram_read8(const BellatrixMemory *m, uint32_t addr)
{
    uint32_t a;

    if (!slow_offset(m, addr, 1u, &a))
        return 0xFFu;
    return m->slow_ram[a];
}

uint16_t slow_ram_read16(const BellatrixMemory *m, uint32_t addr)
{
    uint32_t a;

    if (!slow_offset(m, addr, 2u, &a))
        return 0xFFFFu;
    return ((uint16_t)m->slow_ram[a] << 8) |
           (uint16_t)m->slow_ram[a + 1u];
}

uint32_t slow_ram_read32(const BellatrixMemory *m, uint32_t addr)
{
    uint32_t a;

    if (!slow_offset(m, addr, 4u, &a))
        return 0xFFFFFFFFu;
    return ((uint32_t)m->slow_ram[a] << 24) |
           ((uint32_t)m->slow_ram[a + 1u] << 16) |
           ((uint32_t)m->slow_ram[a + 2u] << 8) |
           (uint32_t)m->slow_ram[a + 3u];
}

/* ------------------------------------------------------------------------- */
/* writes                                                                    */
/* ------------------------------------------------------------------------- */

void slow_ram_write8(BellatrixMemory *m, uint32_t addr, uint8_t value)
{
    uint32_t a;

    if (!slow_offset(m, addr, 1u, &a))
        return;
    m->slow_ram[a] = value;
}

void slow_ram_write16(BellatrixMemory *m, uint32_t addr, uint16_t value)
{
    uint32_t a;

    if (!slow_offset(m, addr, 2u, &a))
        return;
    m->slow_ram[a] = (uint8_t)(value >> 8);
    m->slow_ram[a + 1u] = (uint8_t)(value & 0xFFu);
}

void slow_ram_write32(BellatrixMemory *m, uint32_t addr, uint32_t value)
{
    uint32_t a;

    if (!slow_offset(m, addr, 4u, &a))
        return;
    m->slow_ram[a] = (uint8_t)(value >> 24);
    m->slow_ram[a + 1u] = (uint8_t)((value >> 16) & 0xFFu);
    m->slow_ram[a + 2u] = (uint8_t)((value >> 8) & 0xFFu);
    m->slow_ram[a + 3u] = (uint8_t)(value & 0xFFu);
}
