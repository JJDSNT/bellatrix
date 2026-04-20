#ifndef BELLATRIX_CHIPSET_AGNUS_COPPER_H
#define BELLATRIX_CHIPSET_AGNUS_COPPER_H

#include <stdint.h>

struct AgnusState;

// Copper register offsets (already decoded by bus/router)
#define COPPER_COP1LCH 0x0080u
#define COPPER_COP1LCL 0x0082u
#define COPPER_COP2LCH 0x0084u
#define COPPER_COP2LCL 0x0086u
#define COPPER_COPJMP1 0x0088u
#define COPPER_COPJMP2 0x008Au
#define COPPER_COPINS  0x008Cu

typedef struct CopperState {
    uint32_t cop1lc;
    uint32_t cop2lc;
    uint32_t pc;
} CopperState;

void copper_init(CopperState *c);

void copper_write_reg(CopperState *c, uint16_t reg, uint16_t value);
uint32_t copper_read_reg(CopperState *c, uint16_t reg);

void copper_vbl_execute(CopperState *c, struct AgnusState *agnus);

#endif