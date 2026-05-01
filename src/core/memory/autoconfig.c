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

/* ------------------------------------------------------------------------- */
/* MVP board                                                                 */
/* ------------------------------------------------------------------------- */

static AutoconfigBoard g_board;

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

static inline uint32_t autoconfig_offset(uint32_t addr)
{
    return addr & AUTOCONFIG_Z2_MASK;
}

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void autoconfig_init(BellatrixMemory *m)
{
    (void)m;

    g_board.type = AUTOCONFIG_NONE;
    g_board.enabled = 0;
    g_board.configured = 0;
    g_board.shutup = 1;

    g_board.config_base = AUTOCONFIG_Z2_BASE;
    g_board.assigned_base = 0;
    g_board.size = 0x00800000u;

    g_board.manufacturer = 0;
    g_board.product = 0;
    g_board.flags = 0;

    kprintf("[AUTOCONFIG] disabled; config window=%08x returns empty\n",
            (unsigned)g_board.config_base);
}

void autoconfig_reset(BellatrixMemory *m)
{
    autoconfig_init(m);
}

/* ------------------------------------------------------------------------- */
/* reads                                                                     */
/* ------------------------------------------------------------------------- */

uint8_t autoconfig_read8(BellatrixMemory *m, uint32_t addr)
{
    (void)m;

    (void)addr;
    return 0xFFu;
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

    (void)addr;
    (void)value;
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
