// src/chipset/agnus/copper.c
//
// Copper co-processor — state-machine model driven by beam position.

#include "copper.h"

#include "agnus.h"
#include "beam.h"
#include "memory/memory.h"
#include "support.h"

/* ------------------------------------------------------------------------- */
/* local constants                                                           */
/* ------------------------------------------------------------------------- */

#define CHIP_RAM_MASK 0x001FFFFFu

enum
{
    COPPER_STATE_FETCH_IR1 = 0,
    COPPER_STATE_FETCH_IR2_WAIT_SKIP = 1,
    COPPER_STATE_FETCH_IR2_MOVE = 2,
    COPPER_STATE_WAITING = 3,
    COPPER_STATE_HALTED = 4
};

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

static inline uint16_t copper_read_be16(const AgnusState *agnus, uint32_t addr)
{
    return bellatrix_chip_read16(agnus->memory, addr & CHIP_RAM_MASK);
}

static inline uint16_t copper_current_vhpos(const AgnusState *agnus)
{
    return (uint16_t)(((agnus->beam.vpos & 0xFFu) << 8) |
                      (agnus->beam.hpos & 0xFEu));
}

static inline int copper_beam_past(uint16_t waitpos, uint16_t mask, uint16_t vhpos)
{
    const uint8_t vmask = (mask    >> 8) & 0xFFu;
    const uint8_t vwait = (waitpos >> 8) & 0xFFu;
    const uint8_t hmask =  mask         & 0xFEu;
    const uint8_t hwait =  waitpos      & 0xFEu;
    const uint8_t vcur  = (vhpos   >> 8) & 0xFFu;
    const uint8_t hcur  =  vhpos         & 0xFEu;

    if ((vcur & vmask) > (vwait & vmask))
        return 1;

    if ((vcur & vmask) == (vwait & vmask))
        return ((hcur & hmask) >= (hwait & hmask));

    return 0;
}

static inline int copper_is_illegal_move(const CopperState *c, uint16_t addr)
{
    if (c->cdang)
        return (addr < 0x0040u) ? 1 : 0;

    return (addr < 0x0080u) ? 1 : 0;
}

static inline void copper_fetch_ir1(CopperState *c, const AgnusState *agnus)
{
    c->ir1 = copper_read_be16(agnus, c->pc);
    c->pc = (c->pc + 2u) & CHIP_RAM_MASK;

    if (c->ir1 & 1u)
        c->state = COPPER_STATE_FETCH_IR2_WAIT_SKIP;
    else
        c->state = COPPER_STATE_FETCH_IR2_MOVE;
}

static inline void copper_handle_move(CopperState *c, AgnusState *agnus)
{
    const uint16_t addr = (uint16_t)(c->ir1 & 0x01FEu);

    c->ir2 = copper_read_be16(agnus, c->pc);
    c->pc = (c->pc + 2u) & CHIP_RAM_MASK;

    if (copper_is_illegal_move(c, addr)) {
        kprintf("[COPPER] halt on illegal MOVE addr=%04x value=%04x pc=%05x\n",
                (unsigned)addr,
                (unsigned)c->ir2,
                (unsigned)((c->pc - 4u) & CHIP_RAM_MASK));
        c->state = COPPER_STATE_HALTED;
        return;
    }

    if (addr == AGNUS_BPLCON0) {
        kprintf("[COPPER-MOVE] pc=%05x reg=%04x value=%04x\n",
                (unsigned)((c->pc - 4u) & CHIP_RAM_MASK),
                (unsigned)addr,
                (unsigned)c->ir2);
    }

    agnus_write_reg(agnus, addr, c->ir2, 2);
    c->state = COPPER_STATE_FETCH_IR1;
}

static inline void copper_handle_wait_skip(CopperState *c, AgnusState *agnus)
{
    const uint16_t waitpos = c->ir1;
    const uint16_t vhpos   = copper_current_vhpos(agnus);

    c->ir2 = copper_read_be16(agnus, c->pc);
    c->pc = (c->pc + 2u) & CHIP_RAM_MASK;

    if (waitpos == 0xFFFFu && (c->ir2 & 0xFFFEu) == 0xFFFEu) {
        c->state = COPPER_STATE_HALTED;
        return;
    }

    c->waitpos  = waitpos;
    c->waitmask = (uint16_t)(c->ir2 | 0x0001u);

    if (c->ir2 & 1u) {
        /* SKIP */
        if (copper_beam_past(c->waitpos, c->waitmask, vhpos)) {
            c->pc = (c->pc + 4u) & CHIP_RAM_MASK;
        }
        c->state = COPPER_STATE_FETCH_IR1;
        return;
    }

    /* WAIT */
    if (copper_beam_past(c->waitpos, c->waitmask, vhpos)) {
        c->state = COPPER_STATE_FETCH_IR1;
        return;
    }

    c->state = COPPER_STATE_WAITING;
}

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void copper_init(CopperState *c)
{
    c->cop1lc   = 0;
    c->cop2lc   = 0;
    c->pc       = 0;

    c->ir1      = 0;
    c->ir2      = 0;
    c->waitpos  = 0;
    c->waitmask = 0;

    c->state    = COPPER_STATE_HALTED;
    c->cdang    = 0;
}

void copper_reset(CopperState *c)
{
    copper_init(c);
}

/* ------------------------------------------------------------------------- */
/* register access                                                            */
/* ------------------------------------------------------------------------- */

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
        c->pc = c->cop1lc & CHIP_RAM_MASK & ~1u;
        c->state = COPPER_STATE_FETCH_IR1;
        return;

    case COPPER_COPJMP2:
        c->pc = c->cop2lc & CHIP_RAM_MASK & ~1u;
        c->state = COPPER_STATE_FETCH_IR1;
        return;

    case COPPER_COPINS:
        return;

    case AGNUS_COPCON:
        c->cdang = (value & 0x0002u) ? 1 : 0;
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
    return 0;
}

/* ------------------------------------------------------------------------- */
/* execution                                                                  */
/* ------------------------------------------------------------------------- */

void copper_vbl_reload(CopperState *c)
{
    c->pc = c->cop1lc & CHIP_RAM_MASK & ~1u;
    c->state = COPPER_STATE_FETCH_IR1;
}

void copper_step(CopperState *c, AgnusState *agnus)
{
    if (!agnus || !agnus->memory)
        return;

    switch (c->state) {
    case COPPER_STATE_FETCH_IR1:
        copper_fetch_ir1(c, agnus);
        break;

    case COPPER_STATE_FETCH_IR2_WAIT_SKIP:
        copper_handle_wait_skip(c, agnus);
        break;

    case COPPER_STATE_FETCH_IR2_MOVE:
        copper_handle_move(c, agnus);
        break;

    case COPPER_STATE_WAITING:
        if (copper_beam_past(c->waitpos,
                             c->waitmask,
                             copper_current_vhpos(agnus))) {
            c->state = COPPER_STATE_FETCH_IR1;
        }
        break;

    case COPPER_STATE_HALTED:
    default:
        break;
    }
}