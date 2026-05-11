// src/core/memory/fast_ram.c

#include "memory/fast_ram.h"

#include "debug/cpu_pc.h"
#include "support.h"

/* ------------------------------------------------------------------------- */
/* helpers internos                                                          */
/* ------------------------------------------------------------------------- */

static inline uint32_t fast_addr(const BellatrixMemory *m, uint32_t addr)
{
    return (addr - BELLATRIX_FAST_RAM_BASE) & m->fast_ram_mask;
}

static inline int fast_ram_trace_addr(uint32_t addr)
{
    return addr >= 0x002e0000u && addr < 0x002e0200u;
}

/* ------------------------------------------------------------------------- */
/* reads                                                                     */
/* ------------------------------------------------------------------------- */

uint8_t fast_ram_read8(const BellatrixMemory *m, uint32_t addr)
{
    uint32_t a = fast_addr(m, addr);
    uint8_t v = m->fast_ram[a];

    if (fast_ram_trace_addr(addr))
    {
        kprintf("[FASTRAM-R8] pc=%08x addr=%08x off=%06x val=%02x\n",
                (unsigned)bellatrix_debug_cpu_pc(),
                (unsigned)addr,
                (unsigned)a,
                (unsigned)v);
    }

    return v;
}

uint16_t fast_ram_read16(const BellatrixMemory *m, uint32_t addr)
{
    uint32_t a = fast_addr(m, addr);
    uint16_t v =
        ((uint16_t)m->fast_ram[a] << 8) |
        ((uint16_t)m->fast_ram[(a + 1) & m->fast_ram_mask]);

    if (fast_ram_trace_addr(addr))
    {
        kprintf("[FASTRAM-R16] pc=%08x addr=%08x off=%06x val=%04x\n",
                (unsigned)bellatrix_debug_cpu_pc(),
                (unsigned)addr,
                (unsigned)a,
                (unsigned)v);
    }

    return v;
}

uint32_t fast_ram_read32(const BellatrixMemory *m, uint32_t addr)
{
    uint32_t a = fast_addr(m, addr);
    uint32_t v =
        ((uint32_t)m->fast_ram[a] << 24) |
        ((uint32_t)m->fast_ram[(a + 1) & m->fast_ram_mask] << 16) |
        ((uint32_t)m->fast_ram[(a + 2) & m->fast_ram_mask] << 8)  |
        ((uint32_t)m->fast_ram[(a + 3) & m->fast_ram_mask]);

    if (fast_ram_trace_addr(addr))
    {
        kprintf("[FASTRAM-R32] pc=%08x addr=%08x off=%06x val=%08x\n",
                (unsigned)bellatrix_debug_cpu_pc(),
                (unsigned)addr,
                (unsigned)a,
                (unsigned)v);
    }

    return v;
}

/* ------------------------------------------------------------------------- */
/* writes                                                                    */
/* ------------------------------------------------------------------------- */

void fast_ram_write8(BellatrixMemory *m, uint32_t addr, uint8_t value)
{
    uint32_t a = fast_addr(m, addr);
    m->fast_ram[a] = value;

    if (fast_ram_trace_addr(addr))
    {
        kprintf("[FASTRAM-W8] pc=%08x addr=%08x off=%06x val=%02x\n",
                (unsigned)bellatrix_debug_cpu_pc(),
                (unsigned)addr,
                (unsigned)a,
                (unsigned)value);
    }
}

void fast_ram_write16(BellatrixMemory *m, uint32_t addr, uint16_t value)
{
    uint32_t a = fast_addr(m, addr);

    m->fast_ram[a] = value >> 8;
    m->fast_ram[(a + 1) & m->fast_ram_mask] = value & 0xFF;

    if (fast_ram_trace_addr(addr))
    {
        kprintf("[FASTRAM-W16] pc=%08x addr=%08x off=%06x val=%04x\n",
                (unsigned)bellatrix_debug_cpu_pc(),
                (unsigned)addr,
                (unsigned)a,
                (unsigned)value);
    }
}

void fast_ram_write32(BellatrixMemory *m, uint32_t addr, uint32_t value)
{
    uint32_t a = fast_addr(m, addr);

    m->fast_ram[a] = value >> 24;
    m->fast_ram[(a + 1) & m->fast_ram_mask] = (value >> 16) & 0xFF;
    m->fast_ram[(a + 2) & m->fast_ram_mask] = (value >> 8) & 0xFF;
    m->fast_ram[(a + 3) & m->fast_ram_mask] = value & 0xFF;

    if (fast_ram_trace_addr(addr))
    {
        kprintf("[FASTRAM-W32] pc=%08x addr=%08x off=%06x val=%08x\n",
                (unsigned)bellatrix_debug_cpu_pc(),
                (unsigned)addr,
                (unsigned)a,
                (unsigned)value);
    }
}
