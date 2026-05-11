// src/core/memory/autoconfig.c

#include "memory/autoconfig.h"

#include "support.h"

/* ------------------------------------------------------------------------- */
/* constants                                                                 */
/* ------------------------------------------------------------------------- */

#define AUTOCONFIG_Z2_BASE 0x00E80000u
#define AUTOCONFIG_Z2_MASK 0x0007FFFFu

#define AUTOCONFIG_REG_PRODUCT      0x00u
#define AUTOCONFIG_REG_FLAGS        0x04u
#define AUTOCONFIG_REG_SIZE         0x08u
#define AUTOCONFIG_REG_MANUF_HI     0x10u
#define AUTOCONFIG_REG_MANUF_LO     0x14u
#define AUTOCONFIG_REG_SER_0        0x18u
#define AUTOCONFIG_REG_SER_1        0x1Cu
#define AUTOCONFIG_REG_SER_2        0x20u
#define AUTOCONFIG_REG_SER_3        0x24u
#define AUTOCONFIG_REG_INIT_DIAG    0x28u
#define AUTOCONFIG_REG_ASSIGN_HI    0x48u
#define AUTOCONFIG_REG_ASSIGN_LO    0x4Cu
#define AUTOCONFIG_REG_SHUTUP       0x4Eu

#define AUTOCONFIG_PRODUCT_ID       0x10u
#define AUTOCONFIG_MANUFACTURER_ID  0x6d73u
#define AUTOCONFIG_RAM_SERIAL       0x1e0aeb68u

/* ------------------------------------------------------------------------- */
/* MVP board                                                                 */
/* ------------------------------------------------------------------------- */

static AutoconfigBoard g_board;
static uint8_t g_autoconfig_default_enabled = 0;
static uint32_t g_autoconfig_default_size = 0x00800000u;

static uint16_t g_z2_config[32] = {
    0xe000u, 0x0000u,
    (uint16_t)(~(AUTOCONFIG_PRODUCT_ID << 8) & 0xf000u),
    (uint16_t)(~(AUTOCONFIG_PRODUCT_ID << 12) & 0xf000u),
    (uint16_t)(~0x8fffu), (uint16_t)(~0x0fffu),
    (uint16_t)(~0x0fffu), (uint16_t)(~0x0fffu),

    (uint16_t)(~AUTOCONFIG_MANUFACTURER_ID & 0xf000u),
    (uint16_t)(~(AUTOCONFIG_MANUFACTURER_ID << 4) & 0xf000u),
    (uint16_t)(~(AUTOCONFIG_MANUFACTURER_ID << 8) & 0xf000u),
    (uint16_t)(~(AUTOCONFIG_MANUFACTURER_ID << 12) & 0xf000u),

    (uint16_t)(~(AUTOCONFIG_RAM_SERIAL >> 16) & 0xf000u),
    (uint16_t)(~(AUTOCONFIG_RAM_SERIAL >> 12) & 0xf000u),
    (uint16_t)(~(AUTOCONFIG_RAM_SERIAL >> 8) & 0xf000u),
    (uint16_t)(~(AUTOCONFIG_RAM_SERIAL >> 4) & 0xf000u),
    (uint16_t)(~AUTOCONFIG_RAM_SERIAL & 0xf000u),
    (uint16_t)(~((uint16_t)AUTOCONFIG_RAM_SERIAL << 4) & 0xf000u),
    (uint16_t)(~((uint16_t)AUTOCONFIG_RAM_SERIAL << 8) & 0xf000u),
    (uint16_t)(~((uint16_t)AUTOCONFIG_RAM_SERIAL << 12) & 0xf000u),

    (uint16_t)(~0x0fffu), (uint16_t)(~0x0fffu),
    (uint16_t)(~0x0fffu), (uint16_t)(~0x0fffu),
    (uint16_t)(~0x0fffu), (uint16_t)(~0x0fffu),
    (uint16_t)(~0x0fffu), (uint16_t)(~0x0fffu),
    (uint16_t)(~0x0fffu), (uint16_t)(~0x0fffu),
    (uint16_t)(~0x0fffu), (uint16_t)(~0x0fffu),
};

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

static inline uint32_t autoconfig_offset(uint32_t addr)
{
    return addr & AUTOCONFIG_Z2_MASK;
}

static inline int autoconfig_visible(void)
{
    return g_board.enabled && !g_board.configured && !g_board.shutup;
}

static inline int autoconfig_trace_addr(uint32_t off)
{
    return off <= 0x50u;
}

static inline uint8_t autoconfig_rom_phys_read8(uint32_t off)
{
    uint32_t word_index = off >> 1;
    uint16_t word;

    if (word_index >= (sizeof(g_z2_config) / sizeof(g_z2_config[0])))
        return 0xffu;

    word = g_z2_config[word_index];
    return (off & 1u) ? (uint8_t)(word & 0xffu) : (uint8_t)(word >> 8);
}

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void autoconfig_init(BellatrixMemory *m)
{
    (void)m;

    g_board.type = g_autoconfig_default_enabled ? AUTOCONFIG_Z2 : AUTOCONFIG_NONE;
    g_board.enabled = g_autoconfig_default_enabled;
    g_board.configured = 0;
    g_board.shutup = g_autoconfig_default_enabled ? 0 : 1;

    g_board.config_base = AUTOCONFIG_Z2_BASE;
    g_board.assigned_base = 0;
    g_board.size = g_autoconfig_default_size;

    g_board.manufacturer = g_autoconfig_default_enabled ? AUTOCONFIG_MANUFACTURER_ID : 0;
    g_board.product = g_autoconfig_default_enabled ? AUTOCONFIG_PRODUCT_ID : 0;
    g_board.flags = 0;

    if (g_autoconfig_default_enabled)
    {
        kprintf("[AUTOCONFIG] enabled Z2 RAM size=%uMB window=%08x\n",
                (unsigned)(g_board.size >> 20),
                (unsigned)g_board.config_base);
    }
    else
    {
        kprintf("[AUTOCONFIG] disabled; config window=%08x returns empty\n",
                (unsigned)g_board.config_base);
    }
}

void autoconfig_reset(BellatrixMemory *m)
{
    autoconfig_init(m);
}

void autoconfig_enable_z2_ram(uint32_t size_bytes)
{
    g_autoconfig_default_enabled = 1;
    g_autoconfig_default_size = size_bytes ? size_bytes : 0x00800000u;
}

/* ------------------------------------------------------------------------- */
/* reads                                                                     */
/* ------------------------------------------------------------------------- */

uint8_t autoconfig_read8(BellatrixMemory *m, uint32_t addr)
{
    (void)m;
    uint32_t off = autoconfig_offset(addr);
    uint8_t value = 0xFFu;

    if (autoconfig_visible())
        value = autoconfig_rom_phys_read8(off);

    if (autoconfig_trace_addr(off))
    {
        kprintf("[AUTOCONFIG-R8] addr=%08x off=%02x val=%02x visible=%d cfg=%d shutup=%d\n",
                (unsigned)addr,
                (unsigned)off,
                (unsigned)value,
                autoconfig_visible(),
                g_board.configured,
                g_board.shutup);
    }

    return value;
}

uint16_t autoconfig_read16(BellatrixMemory *m, uint32_t addr)
{
    uint16_t hi = autoconfig_read8(m, addr);
    uint16_t lo = autoconfig_read8(m, addr + 1);

    return (uint16_t)((hi << 8) | lo);
}

uint32_t autoconfig_read32(BellatrixMemory *m, uint32_t addr)
{
    uint32_t b0 = autoconfig_read8(m, addr);
    uint32_t b1 = autoconfig_read8(m, addr + 1);
    uint32_t b2 = autoconfig_read8(m, addr + 2);
    uint32_t b3 = autoconfig_read8(m, addr + 3);

    return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
}

/* ------------------------------------------------------------------------- */
/* writes                                                                    */
/* ------------------------------------------------------------------------- */

void autoconfig_write8(BellatrixMemory *m, uint32_t addr, uint8_t value)
{
    (void)m;
    uint32_t off = autoconfig_offset(addr);

    if (autoconfig_trace_addr(off))
    {
        kprintf("[AUTOCONFIG-W8] addr=%08x off=%02x val=%02x visible=%d cfg=%d shutup=%d\n",
                (unsigned)addr,
                (unsigned)off,
                (unsigned)value,
                autoconfig_visible(),
                g_board.configured,
                g_board.shutup);
    }

    if (!autoconfig_visible())
        return;

    switch (off)
    {
    case AUTOCONFIG_REG_ASSIGN_HI:
        g_board.assigned_base = ((uint32_t)value << 16);
        g_board.configured = 1;
        kprintf("[AUTOCONFIG] assigned Z2 RAM base=%08x size=%uMB\n",
                (unsigned)g_board.assigned_base,
                (unsigned)(g_board.size >> 20));
        break;

    case AUTOCONFIG_REG_SHUTUP:
        g_board.shutup = 1;
        kprintf("[AUTOCONFIG] shutup\n");
        break;

    case (AUTOCONFIG_REG_ASSIGN_HI + 2u):
    case (AUTOCONFIG_REG_SHUTUP + 2u):
        /*
         * AROS m68k-amiga writes autoconfig bytes in the physical nibble-wide
         * layout: offset*4 and offset*4+2. Emu68's own z2ram handler only acts
         * on the primary byte location, so treat the +2 write as shadow.
         */
        break;

    default:
        break;
    }
}

void autoconfig_write16(BellatrixMemory *m, uint32_t addr, uint16_t value)
{
    autoconfig_write8(m, addr, (uint8_t)(value >> 8));
    autoconfig_write8(m, addr + 1, (uint8_t)(value & 0xFF));
}

void autoconfig_write32(BellatrixMemory *m, uint32_t addr, uint32_t value)
{
    autoconfig_write8(m, addr,     (uint8_t)(value >> 24));
    autoconfig_write8(m, addr + 1, (uint8_t)(value >> 16));
    autoconfig_write8(m, addr + 2, (uint8_t)(value >> 8));
    autoconfig_write8(m, addr + 3, (uint8_t)(value & 0xFF));
}
