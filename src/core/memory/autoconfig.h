// src/core/memory/autoconfig.h

#pragma once

#include <stdint.h>

#include "memory/memory.h"

/* ------------------------------------------------------------------------- */
/* Zorro Autoconfig MVP                                                       */
/* ------------------------------------------------------------------------- */

typedef enum
{
    AUTOCONFIG_NONE = 0,
    AUTOCONFIG_Z2,
    AUTOCONFIG_Z3

} AutoconfigType;

typedef struct AutoconfigBoard
{
    AutoconfigType type;

    uint8_t enabled;
    uint8_t configured;
    uint8_t shutup;

    uint32_t config_base;
    uint32_t assigned_base;
    uint32_t size;

    uint16_t manufacturer;
    uint8_t  product;
    uint8_t  flags;

} AutoconfigBoard;

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void autoconfig_init(BellatrixMemory *m);
void autoconfig_reset(BellatrixMemory *m);

/* ------------------------------------------------------------------------- */
/* config-space access                                                       */
/* ------------------------------------------------------------------------- */

uint8_t  autoconfig_read8 (BellatrixMemory *m, uint32_t addr);
uint16_t autoconfig_read16(BellatrixMemory *m, uint32_t addr);
uint32_t autoconfig_read32(BellatrixMemory *m, uint32_t addr);

void autoconfig_write8 (BellatrixMemory *m, uint32_t addr, uint8_t value);
void autoconfig_write16(BellatrixMemory *m, uint32_t addr, uint16_t value);
void autoconfig_write32(BellatrixMemory *m, uint32_t addr, uint32_t value);