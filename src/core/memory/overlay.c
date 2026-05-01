// src/core/memory/overlay.c

#include "memory/overlay.h"
#include "memory/chip_ram.h"

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

static inline int in_low_memory(uint32_t addr)
{
    /*
     * Kickstart overlay only covers the low 512 KiB boot window.
     *
     * Mirroring the whole 2 MiB chip RAM to ROM while OVL=1 makes internal
     * memory reads disagree with the harness CPU bus, which already limits the
     * overlay to 0x000000-0x07ffff.
     */
    return addr < BELLATRIX_ROM_SIZE;
}

static inline int in_rom_window(uint32_t addr)
{
    return addr >= BELLATRIX_ROM_BASE && addr <= BELLATRIX_ROM_END;
}

static inline int rom_offset_from_addr(uint32_t addr, uint32_t *off_out)
{
    if (addr < BELLATRIX_CHIP_RAM_SIZE)
    {
        *off_out = addr;
        return 1;
    }

    if (in_rom_window(addr))
    {
        *off_out = addr - BELLATRIX_ROM_BASE;
        return 1;
    }

    return 0;
}

static inline uint8_t rom_read8(const BellatrixMemory *m, uint32_t addr)
{
    uint32_t off;

    if (!m->rom)
        return 0xFFu;

    if (!rom_offset_from_addr(addr, &off))
        return 0xFFu;

    if (off >= m->rom_size)
        return 0xFFu;

    return m->rom[off];
}

static inline uint16_t rom_read16(const BellatrixMemory *m, uint32_t addr)
{
    return ((uint16_t)rom_read8(m, addr) << 8) |
           ((uint16_t)rom_read8(m, addr + 1u));
}

static inline uint32_t rom_read32(const BellatrixMemory *m, uint32_t addr)
{
    return ((uint32_t)rom_read8(m, addr) << 24) |
           ((uint32_t)rom_read8(m, addr + 1u) << 16) |
           ((uint32_t)rom_read8(m, addr + 2u) << 8) |
           ((uint32_t)rom_read8(m, addr + 3u));
}

/* ------------------------------------------------------------------------- */
/* overlay control                                                           */
/* ------------------------------------------------------------------------- */

void overlay_set(BellatrixMemory *m, int enabled)
{
    m->overlay_enabled = enabled ? 1 : 0;
}

int overlay_enabled(const BellatrixMemory *m)
{
    return m->overlay_enabled;
}

/* ------------------------------------------------------------------------- */
/* reads with overlay                                                        */
/* ------------------------------------------------------------------------- */

uint8_t overlay_read8(const BellatrixMemory *m, uint32_t addr)
{
    if ((overlay_enabled(m) && in_low_memory(addr)) || in_rom_window(addr))
        return rom_read8(m, addr);

    return chip_ram_read8(m, addr);
}

uint16_t overlay_read16(const BellatrixMemory *m, uint32_t addr)
{
    if ((overlay_enabled(m) && in_low_memory(addr)) || in_rom_window(addr))
        return rom_read16(m, addr);

    return chip_ram_read16(m, addr);
}

uint32_t overlay_read32(const BellatrixMemory *m, uint32_t addr)
{
    if ((overlay_enabled(m) && in_low_memory(addr)) || in_rom_window(addr))
        return rom_read32(m, addr);

    return chip_ram_read32(m, addr);
}
