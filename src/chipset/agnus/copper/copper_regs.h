// src/chipset/agnus/copper/copper_regs.h

#pragma once

#include <stdint.h>

typedef struct CopperState CopperState;

/* write / read */

void copper_write_reg(CopperState *c,
                      uint16_t reg,
                      uint16_t value);

uint32_t copper_read_reg(CopperState *c,
                         uint16_t reg);