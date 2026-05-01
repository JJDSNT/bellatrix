// src/bus/gayle/gayle.c

#include "bus/gayle/gayle.h"

#include <string.h>

/*
 * Decodificação simples:
 *
 * AROS acessa:
 *   base + reg * 4
 *
 * Então:
 *   reg = (addr >> 2) & 7
 */

static inline uint32_t decode_reg(uint32_t addr)
{
    return (addr >> 2) & 7u;
}

void gayle_init(
    GayleState *g,
    IsoImage *iso,
    uint8_t *atapi_buffer,
    uint32_t atapi_buffer_size
)
{
    memset(g, 0, sizeof(*g));

    gayle_ide_init(&g->ide, iso, atapi_buffer, atapi_buffer_size);

    /*
     * Qualquer valor != 0 faz ReadGayle() detectar hardware
     */
    g->id = 0x01;
}

void gayle_reset(GayleState *g)
{
    gayle_ide_reset(&g->ide);
}

/* ---------------- MMIO ---------------- */

uint8_t gayle_read8(GayleState *g, uint32_t addr)
{
    uint32_t off = addr - GAYLE_BASE_1200;

    /* ID read (Gayle detection) */
    if (off == 0)
        return gayle_read_id(g);

    uint32_t reg = decode_reg(off);

    return gayle_ide_read8(&g->ide, reg);
}

uint16_t gayle_read16(GayleState *g, uint32_t addr)
{
    uint32_t off = addr - GAYLE_BASE_1200;

    /* DATA port */
    if ((off & 0x3) == 0)
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
    uint32_t off = addr - GAYLE_BASE_1200;

    uint32_t reg = decode_reg(off);

    gayle_ide_write8(&g->ide, reg, val);
}

void gayle_write16(GayleState *g, uint32_t addr, uint16_t val)
{
    uint32_t off = addr - GAYLE_BASE_1200;

    /* DATA port */
    if ((off & 0x3) == 0) {
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

/* ---------------- Gayle ID ---------------- */

uint8_t gayle_read_id(GayleState *g)
{
    return g->id;
}