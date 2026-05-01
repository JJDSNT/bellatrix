// src/chipset/agnus/copper/copper_regs.c

//
// Copper register access — COP1LC, COP2LC, COPJMP, COPCON
//

#include "copper_regs.h"
#include "copper.h"

#include "support.h"
#include "debug/cpu_pc.h"

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

#define CHIP_RAM_MASK 0x001FFFFFu

/* ------------------------------------------------------------------------- */
/* write                                                                     */
/* ------------------------------------------------------------------------- */

void copper_write_reg(CopperState *c, uint16_t reg, uint16_t value)
{
    uint32_t cpu_pc = bellatrix_debug_cpu_pc();

    switch (reg)
    {
    case COPPER_COP1LCH:
    {
        uint32_t old = c->cop1lc & CHIP_RAM_MASK;
        c->cop1lc = (c->cop1lc & 0x0000FFFFu) | ((uint32_t)value << 16);
        kprintf("[COPPER] COP1LCH pc=%08x val=%04x old=0x%06x new=0x%06x\n",
                (unsigned)cpu_pc,
                value,
                (unsigned)old,
                (unsigned)(c->cop1lc & CHIP_RAM_MASK));
        return;
    }

    case COPPER_COP1LCL:
    {
        uint32_t old = c->cop1lc & CHIP_RAM_MASK;
        c->cop1lc = (c->cop1lc & 0xFFFF0000u) | (value & 0xFFFEu);
        kprintf("[COPPER] COP1LCL pc=%08x val=%04x old=0x%06x new=0x%06x\n",
                (unsigned)cpu_pc,
                value,
                (unsigned)old,
                (unsigned)(c->cop1lc & CHIP_RAM_MASK));
        return;
    }

    case COPPER_COP2LCH:
    {
        uint32_t old = c->cop2lc & CHIP_RAM_MASK;
        c->cop2lc = (c->cop2lc & 0x0000FFFFu) | ((uint32_t)value << 16);
        kprintf("[COPPER] COP2LCH pc=%08x val=%04x old=0x%06x new=0x%06x\n",
                (unsigned)cpu_pc,
                value,
                (unsigned)old,
                (unsigned)(c->cop2lc & CHIP_RAM_MASK));
        if (old == 0 && (c->cop2lc & CHIP_RAM_MASK) != 0)
            kprintf("[COPPER-COP2LC-ACTIVE] pc=%08x new=0x%06x\n",
                    (unsigned)cpu_pc,
                    (unsigned)(c->cop2lc & CHIP_RAM_MASK));
        return;
    }

    case COPPER_COP2LCL:
    {
        uint32_t old = c->cop2lc & CHIP_RAM_MASK;
        c->cop2lc = (c->cop2lc & 0xFFFF0000u) | (value & 0xFFFEu);
        kprintf("[COPPER] COP2LCL pc=%08x val=%04x old=0x%06x new=0x%06x\n",
                (unsigned)cpu_pc,
                value,
                (unsigned)old,
                (unsigned)(c->cop2lc & CHIP_RAM_MASK));
        if (old == 0 && (c->cop2lc & CHIP_RAM_MASK) != 0)
            kprintf("[COPPER-COP2LC-ACTIVE] pc=%08x new=0x%06x\n",
                    (unsigned)cpu_pc,
                    (unsigned)(c->cop2lc & CHIP_RAM_MASK));
        return;
    }

    case COPPER_COPJMP1:
    {
        uint32_t old = c->pc;
        copper_restart_program(c, c->cop1lc);

        kprintf("[COPPER-JMP1] pc=%08x old_pc=%05x new_pc=%05x cop1lc=%05x\n",
                (unsigned)cpu_pc,
                (unsigned)old,
                (unsigned)c->pc,
                (unsigned)(c->cop1lc & CHIP_RAM_MASK));
        return;
    }

    case COPPER_COPJMP2:
    {
        uint32_t old = c->pc;
        copper_restart_program(c, c->cop2lc);

        kprintf("[COPPER-JMP2] pc=%08x old_pc=%05x new_pc=%05x cop2lc=%05x\n",
                (unsigned)cpu_pc,
                (unsigned)old,
                (unsigned)c->pc,
                (unsigned)(c->cop2lc & CHIP_RAM_MASK));
        return;
    }

    case AGNUS_COPCON:
        c->cdang = (value & 0x0002u) ? 1 : 0;
        return;

    case COPPER_COPINS:
        return;

    default:
        kprintf("[COPPER] unhandled reg write %04x=%04x\n", reg, value);
        return;
    }
}

/* ------------------------------------------------------------------------- */
/* read                                                                      */
/* ------------------------------------------------------------------------- */

uint32_t copper_read_reg(CopperState *c, uint16_t reg)
{
    switch (reg)
    {
    case COPPER_COP1LCH:
        return (c->cop1lc >> 16) & 0x001Fu;

    case COPPER_COP1LCL:
        return c->cop1lc & 0xFFFEu;

    case COPPER_COP2LCH:
        return (c->cop2lc >> 16) & 0x001Fu;

    case COPPER_COP2LCL:
        return c->cop2lc & 0xFFFEu;

    default:
        return 0;
    }
}
