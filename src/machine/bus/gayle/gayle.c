// src/machine/bus/gayle/gayle.c

#include "machine/bus/gayle/gayle.h"
#include "support.h"

#include <string.h>

/*
 * A1200 GAYLE address map (all relative to $DA0000):
 *
 *   $DA0000-$DA001C  IDE task file (8 regs × 4-byte stride, byte at even addr)
 *   $DA2000          IDE data port (16/32-bit shadow used by AROS driver)
 *   $DA4000+         GAYLE control/status (IRQ $DA9000, INT $DAA000, PCMCIA $DA8000)
 *                    — stubbed: reads return 0, writes ignored
 *   $DE0000-$DEFFFF  GAYLE ID region
 *   $DE1000          GAYLE ID shift register — read by Exec ReadGayle() (LVO -136)
 */

#define GAYLE_CTRL_OFFSET  0x4000u   /* $DA4000+ = control, not IDE task file */

static inline uint32_t decode_reg(uint32_t off)
{
    return (off >> 2) & 7u;
}

static inline bool is_de_region(uint32_t addr)
{
    return addr >= GAYLE_ID_REGION_BASE &&
           addr <  GAYLE_ID_REGION_BASE + GAYLE_ID_REGION_SIZE;
}

/* ---------------- GAYLE ID shift register ---------------- */

/*
 * A1200 GAYLE ID = 0xD (binary 1101).
 * Each byte read of $DE1000 returns the current MSB in bit 7, then shifts.
 * ReadGayle() (KS LVO -136) does 4 reads and assembles the nibble.
 */
static uint8_t gayle_id_shift_read(GayleState *g)
{
    /* Ring counter: reload after every complete 4-bit cycle so that
     * repeated ReadGayle() calls (rom_init.S + probe.c) all see 0xD. */
    if (g->id_shift == 0)
        g->id_shift = 0xDu;

    uint8_t bit = (g->id_shift >> 3) & 1u;
    g->id_shift = (g->id_shift << 1) & 0x0Fu;
    uint8_t result = bit ? 0x80u : 0x00u;
    kprintf("[GAYLE-ID] DE1000 read -> %02x (remaining shift=%x)\n",
            (unsigned)result, (unsigned)g->id_shift);
    return result;
}

/* ---------------- Lifecycle ---------------- */

void gayle_init(
    GayleState *g,
    IsoImage *iso,
    uint8_t *atapi_buffer,
    uint32_t atapi_buffer_size
)
{
    memset(g, 0, sizeof(*g));

    gayle_ide_init(&g->ide, iso, atapi_buffer, atapi_buffer_size);

    g->id_shift = 0xDu;
}

void gayle_reset(GayleState *g)
{
    gayle_ide_reset(&g->ide);
    g->id_shift = 0xDu;
}

/* ---------------- MMIO ---------------- */

uint8_t gayle_read8(GayleState *g, uint32_t addr)
{
    /* $DE1000: GAYLE ID shift register (read by ReadGayle / Exec LVO -136) */
    if (is_de_region(addr)) {
        if (addr == GAYLE_ID_REG)
            return gayle_id_shift_read(g);
        return 0;
    }

    uint32_t off = addr - GAYLE_BASE_1200;

    /* $DA4000+: control/status block.
     * $DA9000 (GAYLE_IRQ): bit 7 = IDE interrupt pending (GAYLE_IRQ_IDE=0x80).
     * All other control registers stub to 0. */
    if (off >= GAYLE_CTRL_OFFSET) {
        if (addr == 0x00DA9000u)
            return gayle_ide_irq_pending(&g->ide) ? 0x80u : 0x00u;
        return 0;
    }

    return gayle_ide_read8(&g->ide, decode_reg(off));
}

uint16_t gayle_read16(GayleState *g, uint32_t addr)
{
    if (is_de_region(addr)) {
        uint8_t hi = gayle_read8(g, addr);
        uint8_t lo = gayle_read8(g, addr + 1);
        return ((uint16_t)hi << 8) | lo;
    }

    uint32_t off = addr - GAYLE_BASE_1200;

    if (off >= GAYLE_CTRL_OFFSET)
        return 0;

    /* DATA port: 16-bit transfer path.
     * Both $DA0000 (slow 8-bit alias) and $DA2000 (fast 16-bit shadow) map here
     * because decode_reg of both gives reg=0 and the alignment check passes. */
    if ((off & 0x3u) == 0)
        return gayle_ide_read16(&g->ide);

    uint8_t hi = gayle_read8(g, addr);
    uint8_t lo = gayle_read8(g, addr + 1);
    return ((uint16_t)hi << 8) | lo;
}

uint32_t gayle_read32(GayleState *g, uint32_t addr)
{
    uint16_t hi = gayle_read16(g, addr);
    uint16_t lo = gayle_read16(g, addr + 2);
    return ((uint32_t)hi << 16) | lo;
}

void gayle_write8(GayleState *g, uint32_t addr, uint8_t val)
{
    if (is_de_region(addr))
        return;

    uint32_t off = addr - GAYLE_BASE_1200;

    if (off >= GAYLE_CTRL_OFFSET) {
        /* $DA9000: AROS clears IDE IRQ by writing 0x7c (leaves low bits, clears bit 7).
         * Bit 7 clear in the written value = acknowledge IDE interrupt. */
        if (addr == 0x00DA9000u && !(val & 0x80u))
            gayle_ide_clear_irq(&g->ide);
        return;
    }

    gayle_ide_write8(&g->ide, decode_reg(off), val);
}

void gayle_write16(GayleState *g, uint32_t addr, uint16_t val)
{
    if (is_de_region(addr))
        return;

    uint32_t off = addr - GAYLE_BASE_1200;

    if (off >= GAYLE_CTRL_OFFSET)
        return;

    /* DATA port */
    if ((off & 0x3u) == 0) {
        gayle_ide_write16(&g->ide, val);
        return;
    }

    gayle_write8(g, addr,     (val >> 8) & 0xff);
    gayle_write8(g, addr + 1, val & 0xff);
}

void gayle_write32(GayleState *g, uint32_t addr, uint32_t val)
{
    gayle_write16(g, addr,     (val >> 16) & 0xffff);
    gayle_write16(g, addr + 2, val & 0xffff);
}

/* ---------------- Unified access ---------------- */

bool gayle_owns_address(const GayleState *g, uint32_t addr)
{
    (void)g;
    /* IDE + control registers: $DA0000-$DAFFFF */
    if (addr >= GAYLE_BASE_1200 && addr < GAYLE_BASE_1200 + GAYLE_SIZE)
        return true;
    /* GAYLE ID region: $DE0000-$DEFFFF */
    if (addr >= GAYLE_ID_REGION_BASE && addr < GAYLE_ID_REGION_BASE + GAYLE_ID_REGION_SIZE)
        return true;
    return false;
}

uint32_t gayle_read(GayleState *g, uint32_t addr, unsigned size)
{
    switch (size) {
    case 1:  return (uint32_t)gayle_read8 (g, addr);
    case 2:  return (uint32_t)gayle_read16(g, addr);
    case 4:  return            gayle_read32(g, addr);
    default: return 0xffffffffu;
    }
}

void gayle_write(GayleState *g, uint32_t addr, uint32_t value, unsigned size)
{
    switch (size) {
    case 1: gayle_write8 (g, addr, (uint8_t)value);  break;
    case 2: gayle_write16(g, addr, (uint16_t)value); break;
    case 4: gayle_write32(g, addr, value);            break;
    default: break;
    }
}
