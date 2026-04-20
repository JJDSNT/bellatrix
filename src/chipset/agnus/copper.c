// src/chipset/agnus/copper.c
//
// Copper co-processor — simplified VBL-batch model.
//
// Current model:
//   - At each VBL, PC is reloaded from COP1LC.
//   - MOVE instructions write directly to Agnus-owned register API.
//   - WAIT/SKIP are executed through, except the standard end sentinel
//     FFFF / FFFE which stops execution.
//   - This is enough for static-frame setup before final render.
//
// Future:
//   - Convert to scanline/raster scheduler if mid-frame copper effects are needed.

#include "copper.h"

#include "agnus.h"
#include "support.h"

#define CHIP_RAM_VIRT  0xffffff9000000000ULL
#define CHIP_RAM_MASK  0x001FFFFFUL   // 2 MB

static inline uint16_t chip_read16(uint32_t addr)
{
    return *(const volatile uint16_t *)(CHIP_RAM_VIRT + (addr & CHIP_RAM_MASK));
}

void copper_init(CopperState *c)
{
    c->cop1lc = 0;
    c->cop2lc = 0;
    c->pc     = 0;
}

void copper_write_reg(CopperState *c, uint16_t reg, uint16_t value)
{
    switch (reg) {
    case COPPER_COP1LCH:
        c->cop1lc = (c->cop1lc & 0x0000FFFFu) | ((uint32_t)(value & 0x001Fu) << 16);
        return;

    case COPPER_COP1LCL:
        c->cop1lc = (c->cop1lc & 0x001F0000u) | (uint32_t)(value & 0xFFFEu);
        return;

    case COPPER_COP2LCH:
        c->cop2lc = (c->cop2lc & 0x0000FFFFu) | ((uint32_t)(value & 0x001Fu) << 16);
        return;

    case COPPER_COP2LCL:
        c->cop2lc = (c->cop2lc & 0x001F0000u) | (uint32_t)(value & 0xFFFEu);
        return;

    case COPPER_COPJMP1:
        c->pc = c->cop1lc;
        return;

    case COPPER_COPJMP2:
        c->pc = c->cop2lc;
        return;

    case COPPER_COPINS:
        // no persistent behavior in this simplified model
        return;

    default:
        kprintf("[COPPER] unhandled write reg=%04x value=%04x\n",
                (unsigned)reg,
                (unsigned)value);
        return;
    }
}

uint32_t copper_read_reg(CopperState *c, uint16_t reg)
{
    (void)c;
    (void)reg;

    // Copper regs are effectively write-only for our current model.
    return 0;
}

void copper_vbl_execute(CopperState *c, AgnusState *agnus)
{
    uint32_t pc = c->cop1lc;

    if (!pc) {
        return;
    }

    // Safety guard against corrupted lists / loops.
    int limit = 8192;

    pc &= CHIP_RAM_MASK & ~1u;
    c->pc = pc;

    while (limit-- > 0) {
        if ((pc + 3u) > CHIP_RAM_MASK) {
            break;
        }

        uint16_t ir1 = chip_read16(pc);
        uint16_t ir2 = chip_read16(pc + 2u);
        pc += 4u;

        if (!(ir1 & 1u)) {
            // MOVE
            uint16_t reg = (uint16_t)(ir1 & 0x01FEu);

            // DANGER bit emulation simplificada:
            // só permitimos writes a partir de $40.
            if (reg >= 0x0040u) {
                agnus_write_reg(agnus, reg, ir2, 2);
            }
        } else {
            // WAIT or SKIP
            if (ir1 == 0xFFFFu && (ir2 & 0xFFFEu) == 0xFFFEu) {
                break;
            }

            // Other WAIT/SKIP are ignored in this simplified phase.
        }
    }

    c->pc = pc;
}