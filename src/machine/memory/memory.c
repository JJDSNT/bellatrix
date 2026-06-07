// src/machine/memory/memory.c

#include "machine/memory/memory.h"
#include "machine/memory/memory_map.h"
#include "machine/memory/chip_ram.h"
#include "machine/memory/fast_ram.h"
#include "machine/memory/slow_ram.h"
#include "machine/memory/overlay.h"
#include "support.h"

#if defined(BELLATRIX_HARNESS)
#include "machine/bus/zorro2/zorro2_bus.h"
#endif

#include <string.h>
#if defined(BELLATRIX_HARNESS)
#include <stdlib.h>
#endif

/* ------------------------------------------------------------------------- */
/* default backing                                                           */
/* ------------------------------------------------------------------------- */

#define BELLATRIX_CHIP_RAM_VIRT ((uint8_t *)0xffffff9000000000ULL)

#if defined(BELLATRIX_HARNESS)
#define BELLATRIX_FAST_RAM_SIZE 0x00800000u
#define BELLATRIX_FAST_RAM_MASK 0x007FFFFFu

static uint8_t g_fast_ram[BELLATRIX_FAST_RAM_SIZE];
static uint8_t g_slow_ram[BELLATRIX_SLOW_RAM_SIZE];
#endif

static void bellatrix_memory_log_expansion_backend(void)
{
#if defined(BELLATRIX_HARNESS)
    kprintf("[MEMMAP] expansion backend: harness zorro2 bus\n");
#elif defined(BELLATRIX_EMU68)
    kprintf("[MEMMAP] expansion backend: emu68 boards\n");
#endif
}

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void bellatrix_memory_init(BellatrixMemory *m)
{
    memset(m, 0, sizeof(*m));

    m->chip_ram = BELLATRIX_CHIP_RAM_VIRT;
    m->chip_ram_size = BELLATRIX_CHIP_RAM_SIZE;
    m->chip_ram_mask = BELLATRIX_CHIP_RAM_MASK;

#if defined(BELLATRIX_HARNESS)
    m->fast_ram = g_fast_ram;
    m->fast_ram_size = BELLATRIX_FAST_RAM_SIZE;
    m->fast_ram_mask = BELLATRIX_FAST_RAM_MASK;
#else
    m->fast_ram = 0;
    m->fast_ram_size = 0;
    m->fast_ram_mask = 0;
#endif

#if defined(BELLATRIX_HARNESS)
    {
        const char *slow = getenv("BELLATRIX_SLOW_RAM");
        m->slow_ram = g_slow_ram;
        m->slow_ram_size = BELLATRIX_SLOW_RAM_SIZE;
        m->slow_ram_enabled = !(slow && slow[0] == '0');
    }
#else
    m->slow_ram = 0;
    m->slow_ram_size = 0;
    m->slow_ram_enabled = 0;
#endif

    m->rom = 0;
    m->rom_size = 0;
    m->rom_ext = 0;
    m->rom_ext_size = 0;

    m->overlay_enabled = 1;

#if defined(BELLATRIX_HARNESS)
    memset(m->fast_ram, 0, m->fast_ram_size);
    memset(m->slow_ram, 0, m->slow_ram_size);
    if (m->slow_ram_enabled) {
        kprintf("[MEMMAP] slow RAM: 0x%06x-0x%06x (%u KB)\n",
                (unsigned)BELLATRIX_SLOW_RAM_BASE,
                (unsigned)(BELLATRIX_SLOW_RAM_BASE + m->slow_ram_size - 1u),
                (unsigned)(m->slow_ram_size / 1024u));
    }
#endif

    bellatrix_memory_log_expansion_backend();
}

void bellatrix_memory_reset(BellatrixMemory *m)
{
    bellatrix_memory_init(m);

}

void bellatrix_memory_attach_rom(BellatrixMemory *m,
                                 const uint8_t *rom,
                                 size_t rom_size)
{
    m->rom = rom;
    m->rom_size = rom_size;
}

void bellatrix_memory_attach_ext_rom(BellatrixMemory *m,
                                     const uint8_t *rom_ext,
                                     size_t rom_ext_size)
{
    m->rom_ext = rom_ext;
    m->rom_ext_size = rom_ext_size;
}

/* ------------------------------------------------------------------------- */
/* overlay                                                                   */
/* ------------------------------------------------------------------------- */

void bellatrix_memory_set_overlay(BellatrixMemory *m, int enabled)
{
    overlay_set(m, enabled);
}

int bellatrix_memory_overlay_enabled(const BellatrixMemory *m)
{
    return overlay_enabled(m);
}

/* ------------------------------------------------------------------------- */
/* generic machine memory API                                                */
/* ------------------------------------------------------------------------- */

uint8_t bellatrix_mem_read8(BellatrixMemory *m, uint32_t addr)
{
    return memory_map_read8(m, addr);
}

uint16_t bellatrix_mem_read16(BellatrixMemory *m, uint32_t addr)
{
    return memory_map_read16(m, addr);
}

uint32_t bellatrix_mem_read32(BellatrixMemory *m, uint32_t addr)
{
    return memory_map_read32(m, addr);
}

void bellatrix_mem_write8(BellatrixMemory *m, uint32_t addr, uint8_t value)
{
    memory_map_write8(m, addr, value);
}

void bellatrix_mem_write16(BellatrixMemory *m, uint32_t addr, uint16_t value)
{
    memory_map_write16(m, addr, value);
}

void bellatrix_mem_write32(BellatrixMemory *m, uint32_t addr, uint32_t value)
{
    memory_map_write32(m, addr, value);
}

/* ------------------------------------------------------------------------- */
/* direct chip RAM API                                                       */
/* ------------------------------------------------------------------------- */

uint8_t bellatrix_chip_read8(const BellatrixMemory *m, uint32_t addr)
{
    return chip_ram_read8(m, addr);
}

uint16_t bellatrix_chip_read16(const BellatrixMemory *m, uint32_t addr)
{
    return chip_ram_read16(m, addr);
}

uint32_t bellatrix_chip_read32(const BellatrixMemory *m, uint32_t addr)
{
    return chip_ram_read32(m, addr);
}

void bellatrix_chip_write8(BellatrixMemory *m, uint32_t addr, uint8_t value)
{
    chip_ram_write8(m, addr, value);
}

void bellatrix_chip_write16(BellatrixMemory *m, uint32_t addr, uint16_t value)
{
    chip_ram_write16(m, addr, value);
}

void bellatrix_chip_write32(BellatrixMemory *m, uint32_t addr, uint32_t value)
{
    chip_ram_write32(m, addr, value);
}

/* ------------------------------------------------------------------------- */
/* direct fast RAM API                                                       */
/* ------------------------------------------------------------------------- */

uint8_t bellatrix_fast_read8(const BellatrixMemory *m, uint32_t addr)
{
    if (!m || !m->fast_ram || !m->fast_ram_size) {
        return 0xFFu;
    }
    return fast_ram_read8(m, addr);
}

uint16_t bellatrix_fast_read16(const BellatrixMemory *m, uint32_t addr)
{
    if (!m || !m->fast_ram || !m->fast_ram_size) {
        return 0xFFFFu;
    }
    return fast_ram_read16(m, addr);
}

uint32_t bellatrix_fast_read32(const BellatrixMemory *m, uint32_t addr)
{
    if (!m || !m->fast_ram || !m->fast_ram_size) {
        return 0xFFFFFFFFu;
    }
    return fast_ram_read32(m, addr);
}

void bellatrix_fast_write8(BellatrixMemory *m, uint32_t addr, uint8_t value)
{
    if (!m || !m->fast_ram || !m->fast_ram_size) {
        return;
    }
    fast_ram_write8(m, addr, value);
}

void bellatrix_fast_write16(BellatrixMemory *m, uint32_t addr, uint16_t value)
{
    if (!m || !m->fast_ram || !m->fast_ram_size) {
        return;
    }
    fast_ram_write16(m, addr, value);
}

void bellatrix_fast_write32(BellatrixMemory *m, uint32_t addr, uint32_t value)
{
    if (!m || !m->fast_ram || !m->fast_ram_size) {
        return;
    }
    fast_ram_write32(m, addr, value);
}

/* ------------------------------------------------------------------------- */
/* direct slow RAM API                                                       */
/* ------------------------------------------------------------------------- */

uint8_t bellatrix_slow_read8(const BellatrixMemory *m, uint32_t addr)
{
    return slow_ram_read8(m, addr);
}

uint16_t bellatrix_slow_read16(const BellatrixMemory *m, uint32_t addr)
{
    return slow_ram_read16(m, addr);
}

uint32_t bellatrix_slow_read32(const BellatrixMemory *m, uint32_t addr)
{
    return slow_ram_read32(m, addr);
}

void bellatrix_slow_write8(BellatrixMemory *m, uint32_t addr, uint8_t value)
{
    slow_ram_write8(m, addr, value);
}

void bellatrix_slow_write16(BellatrixMemory *m, uint32_t addr, uint16_t value)
{
    slow_ram_write16(m, addr, value);
}

void bellatrix_slow_write32(BellatrixMemory *m, uint32_t addr, uint32_t value)
{
    slow_ram_write32(m, addr, value);
}

int bellatrix_slow_contains(const BellatrixMemory *m, uint32_t addr, unsigned int size)
{
    return slow_ram_contains(m, addr, size);
}

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

uint32_t bellatrix_chip_wrap_addr(const BellatrixMemory *m, uint32_t addr)
{
    return chip_ram_wrap_addr(m, addr);
}

int bellatrix_chip_is_configured(const BellatrixMemory *m)
{
    return chip_ram_is_configured(m);
}
