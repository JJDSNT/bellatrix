// src/machine/memory/chip_ram.c

#include "machine/memory/chip_ram.h"
#include "support.h"

#ifdef BELLATRIX_HARNESS
#include "debug/cpu_pc.h"

#include <stdlib.h>
#endif

/* ------------------------------------------------------------------------- */
/* helpers internos                                                          */
/* ------------------------------------------------------------------------- */

static inline uint32_t chip_addr(const BellatrixMemory *m, uint32_t addr)
{
    return addr & m->chip_ram_mask;
}

#ifdef BELLATRIX_HARNESS
static void chip_ram_watch_write(uint32_t addr, int size, uint32_t value)
{
    static int init;
    static uint32_t lo;
    static uint32_t hi;
    uint32_t end;

    if (!init) {
        const char *spec = getenv("BELLATRIX_CHIP_WRITE_WATCH");
        char *endptr = NULL;
        init = 1;
        if (spec && *spec) {
            unsigned long parsed_lo = strtoul(spec, &endptr, 0);
            if (endptr && *endptr == ':') {
                unsigned long parsed_hi = strtoul(endptr + 1, &endptr, 0);
                lo = (uint32_t)parsed_lo & 0x00ffffffu;
                hi = (uint32_t)parsed_hi & 0x00ffffffu;
            }
        }
    }

    if (lo >= hi)
        return;

    addr &= 0x00ffffffu;
    end = addr + (uint32_t)size;
    if (end <= lo || addr >= hi)
        return;

    kprintf("[CHIP-WATCH-W%d] addr=%06x value=%08x pc=%08x\n",
            size * 8,
            (unsigned)addr,
            (unsigned)value,
            (unsigned)bellatrix_debug_cpu_pc());
}
#else
static inline void chip_ram_watch_write(uint32_t addr, int size, uint32_t value)
{
    (void)addr;
    (void)size;
    (void)value;
}
#endif

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
    chip_ram_watch_write(addr, 1, value);
    m->chip_ram[a] = value;
}

void chip_ram_write16(BellatrixMemory *m, uint32_t addr, uint16_t value)
{
    uint32_t a = chip_addr(m, addr);
    chip_ram_watch_write(addr, 2, value);
    m->chip_ram[a] = value >> 8;
    m->chip_ram[(a + 1) & m->chip_ram_mask] = value & 0xFF;
}

void chip_ram_write32(BellatrixMemory *m, uint32_t addr, uint32_t value)
{
    uint32_t a = chip_addr(m, addr);
    chip_ram_watch_write(addr, 4, value);
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
