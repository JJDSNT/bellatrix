// src/bus/gayle/gayle.h

#ifndef BELLATRIX_BUS_GAYLE_H
#define BELLATRIX_BUS_GAYLE_H

#include <stdint.h>
#include <stdbool.h>

#include "bus/gayle/gayle_ide.h"

#define GAYLE_BASE_1200   0x00DA0000u
#define GAYLE_SIZE        0x10000u

typedef struct GayleState {
    GayleIde ide;

    /* status simples do Gayle */
    uint8_t id;          // usado por ReadGayle()
} GayleState;

void gayle_init(
    GayleState *g,
    IsoImage *iso,
    uint8_t *atapi_buffer,
    uint32_t atapi_buffer_size
);

void gayle_reset(GayleState *g);

/* MMIO */
uint8_t  gayle_read8 (GayleState *g, uint32_t addr);
uint16_t gayle_read16(GayleState *g, uint32_t addr);
uint32_t gayle_read32(GayleState *g, uint32_t addr);

void gayle_write8 (GayleState *g, uint32_t addr, uint8_t  val);
void gayle_write16(GayleState *g, uint32_t addr, uint16_t val);
void gayle_write32(GayleState *g, uint32_t addr, uint32_t val);

/* usado pelo AROS */
uint8_t gayle_read_id(GayleState *g);

#endif