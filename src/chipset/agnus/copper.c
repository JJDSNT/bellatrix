// src/chipset/agnus/copper.c
//
// Copper co-processor — state-machine model driven by beam position.

#include "copper.h"

#include "agnus.h"
#include "beam.h"
#include "blitter.h"
#include "memory/memory.h"
#include "support.h"

/* ------------------------------------------------------------------------- */
/* local constants                                                           */
/* ------------------------------------------------------------------------- */

#define CHIP_RAM_MASK 0x001FFFFFu

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

static inline int copper_is_move_cmd(uint16_t ir1)
{
    return (ir1 & 1u) ? 0 : 1;
}

static inline int copper_is_skip_cmd(uint16_t ir1, uint16_t ir2)
{
    return (ir1 & 1u) && (ir2 & 1u);
}

static inline int copper_is_wait_cmd(uint16_t ir1, uint16_t ir2)
{
    return (ir1 & 1u) && !(ir2 & 1u);
}

static inline uint16_t copper_get_ra(uint16_t ir1)
{
    return (uint16_t)(ir1 & 0x01FEu);
}

static inline uint16_t copper_get_dw(uint16_t ir2)
{
    return ir2;
}

static inline uint16_t copper_get_waitpos(uint16_t ir1)
{
    return (uint16_t)(ir1 & 0xFFFEu);
}

static inline uint16_t copper_get_waitmask(uint16_t ir2)
{
    /*
     * Hardware forces bit 0 and bit 15.
     * This matches the classic VMHM handling.
     */
    return (uint16_t)((ir2 & 0x7FFEu) | 0x8001u);
}

static inline int copper_get_bfd(uint16_t ir2)
{
    return (ir2 & 0x8000u) ? 1 : 0;
}

static inline int copper_is_illegal_move(const CopperState *c, uint16_t addr)
{
    /*
     * Match your current bring-up policy:
     * - odd address is illegal
     * - outside custom register window is illegal
     * - if CDANG is off, deny below 0x80
     * - if CDANG is on, allow down to 0x40 (OCS-style bring-up rule)
     */
    if (addr & 1u)
        return 1;

    if (addr > 0x01FEu)
        return 1;

    if (c->cdang) {
        if (addr < 0x0040u)
            return 1;
    } else {
        if (addr < 0x0080u)
            return 1;
    }

    return 0;
}

static inline int copper_wait_vertical_match(uint16_t waitpos,
                                             uint16_t waitmask,
                                             uint16_t vhpos)
{
    const uint8_t vmask = (uint8_t)((waitmask >> 8) & 0xFFu);
    const uint8_t vwait = (uint8_t)((waitpos  >> 8) & 0xFFu);
    const uint8_t vcur  = (uint8_t)((vhpos    >> 8) & 0xFFu);

    if ((vcur & vmask) > (vwait & vmask))
        return 1;

    if ((vcur & vmask) < (vwait & vmask))
        return 0;

    return 2; /* equal, horizontal compare still needed */
}

static inline int copper_wait_horizontal_match(uint16_t waitpos,
                                               uint16_t waitmask,
                                               uint16_t vhpos)
{
    /*
     * Important: emulate the Copper horizontal comparator with the classic
     * +2 cycle bias before EOL and wrapped behavior after E0.
     *
     * This is the main thing your current comparator is missing.
     */
    uint8_t hmask = (uint8_t)(waitmask & 0xFEu);
    uint8_t hwait = (uint8_t)(waitpos  & 0xFEu);
    uint8_t hcur  = (uint8_t)(vhpos    & 0xFEu);
    uint8_t hcmp;

    if (hcur < 0xE0u)
        hcmp = (uint8_t)((hcur + 2u) & 0xFEu);
    else
        hcmp = (uint8_t)((hcur - 0xE0u) & 0xFEu);

    return ((hcmp & hmask) >= (hwait & hmask)) ? 1 : 0;
}

static inline int copper_wait_matches(uint16_t waitpos,
                                      uint16_t waitmask,
                                      uint16_t vhpos)
{
    int vcmp = copper_wait_vertical_match(waitpos, waitmask, vhpos);

    if (vcmp == 1)
        return 1;

    if (vcmp == 0)
        return 0;

    return copper_wait_horizontal_match(waitpos, waitmask, vhpos);
}

static inline int copper_blitter_busy(const AgnusState *agnus)
{
    return blitter_is_busy(&agnus->blitter);
}

static inline void copper_fetch_ir1(CopperState *c, const AgnusState *agnus)
{
    c->ir1 = copper_read_be16(agnus, c->pc);
    c->pc = (c->pc + 2u) & CHIP_RAM_MASK;

    if (copper_is_move_cmd(c->ir1))
        c->state = COPPER_STATE_FETCH_IR2_MOVE;
    else
        c->state = COPPER_STATE_FETCH_IR2_WAIT_SKIP;
}

static inline void copper_handle_move(CopperState *c, AgnusState *agnus)
{
    const uint16_t addr = copper_get_ra(c->ir1);

    c->ir2 = copper_read_be16(agnus, c->pc);
    c->pc = (c->pc + 2u) & CHIP_RAM_MASK;

    if (copper_is_illegal_move(c, addr))
    {
        kprintf("[COPPER] skip illegal MOVE ir1=%04x addr=%04x value=%04x pc=%05x\n",
                (unsigned)c->ir1,
                (unsigned)addr,
                (unsigned)c->ir2,
                (unsigned)((c->pc - 4u) & CHIP_RAM_MASK));
        c->state = COPPER_STATE_FETCH_IR1;
        return;
    }

    kprintf("[COPPER-MOVE] pc=%05x reg=%04x value=%04x\n",
            (unsigned)((c->pc - 4u) & CHIP_RAM_MASK),
            (unsigned)addr,
            (unsigned)copper_get_dw(c->ir2));

    agnus_write_reg(agnus, addr, copper_get_dw(c->ir2), 2);
    c->state = COPPER_STATE_FETCH_IR1;
}

static inline void copper_handle_skip(CopperState *c, AgnusState *agnus)
{
    const uint16_t vhpos = copper_current_vhpos(agnus);
    int skip = copper_wait_matches(c->waitpos, c->waitmask, vhpos);

    kprintf("[COPPER-SKIP] pc=%05x ir1=%04x ir2=%04x vhpos=%04x %s\n",
            (unsigned)((c->pc - 4u) & CHIP_RAM_MASK),
            (unsigned)c->ir1,
            (unsigned)c->ir2,
            (unsigned)vhpos,
            skip ? "TAKEN" : "not-taken");

    if (skip)
        c->pc = (c->pc + 4u) & CHIP_RAM_MASK;

    c->state = COPPER_STATE_FETCH_IR1;
}

static inline void copper_enter_wait(CopperState *c, AgnusState *agnus)
{
    const uint16_t vhpos = copper_current_vhpos(agnus);

    /*
     * WAIT with BFD clear means: also wait for blitter to become idle.
     * WAIT with BFD set means: ignore blitter busy and only wait for raster.
     */
    if (!c->wait_bfd && copper_blitter_busy(agnus)) {
        kprintf("[COPPER-WAIT] pc=%05x ir1=%04x ir2=%04x vhpos=%04x -> WAIT_BLITTER\n",
                (unsigned)((c->pc - 4u) & CHIP_RAM_MASK),
                (unsigned)c->ir1,
                (unsigned)c->ir2,
                (unsigned)vhpos);
        c->state = COPPER_STATE_WAITING_BLITTER;
        return;
    }

    if (copper_wait_matches(c->waitpos, c->waitmask, vhpos)) {
        kprintf("[COPPER-WAIT] pc=%05x ir1=%04x ir2=%04x vhpos=%04x -> PASS\n",
                (unsigned)((c->pc - 4u) & CHIP_RAM_MASK),
                (unsigned)c->ir1,
                (unsigned)c->ir2,
                (unsigned)vhpos);
        c->state = COPPER_STATE_FETCH_IR1;
        return;
    }

    kprintf("[COPPER-WAIT] pc=%05x ir1=%04x ir2=%04x vhpos=%04x -> WAIT_RASTER\n",
            (unsigned)((c->pc - 4u) & CHIP_RAM_MASK),
            (unsigned)c->ir1,
            (unsigned)c->ir2,
            (unsigned)vhpos);

    c->state = COPPER_STATE_WAITING_RASTER;
}

static inline void copper_handle_wait_skip(CopperState *c, AgnusState *agnus)
{
    c->ir2 = copper_read_be16(agnus, c->pc);
    c->pc  = (c->pc + 2u) & CHIP_RAM_MASK;

    /*
     * End marker / halt pattern
     */
    if (c->ir1 == 0xFFFFu && (c->ir2 & 0xFFFEu) == 0xFFFEu) {
        kprintf("[COPPER-HALT] pc=%05x\n",
                (unsigned)((c->pc - 4u) & CHIP_RAM_MASK));
        c->state = COPPER_STATE_HALTED;
        return;
    }

    c->waitpos  = copper_get_waitpos(c->ir1);
    c->waitmask = copper_get_waitmask(c->ir2);
    c->wait_bfd = copper_get_bfd(c->ir2) ? 1 : 0;

    if (copper_is_skip_cmd(c->ir1, c->ir2)) {
        copper_handle_skip(c, agnus);
        return;
    }

    if (copper_is_wait_cmd(c->ir1, c->ir2)) {
        copper_enter_wait(c, agnus);
        return;
    }

    /*
     * Should never happen, but keep the machine alive.
     */
    c->state = COPPER_STATE_FETCH_IR1;
}

static inline void copper_poll_wait(CopperState *c, AgnusState *agnus)
{
    const uint16_t vhpos = copper_current_vhpos(agnus);

    if (c->state == COPPER_STATE_WAITING_BLITTER) {
        if (copper_blitter_busy(agnus))
            return;

        /*
         * Once blitter is done, still honor raster wait.
         */
        if (copper_wait_matches(c->waitpos, c->waitmask, vhpos)) {
            kprintf("[COPPER] blitter wake pc=%05x vhpos=%04x -> PASS\n",
                    (unsigned)c->pc,
                    (unsigned)vhpos);
            c->state = COPPER_STATE_FETCH_IR1;
        } else {
            kprintf("[COPPER] blitter wake pc=%05x vhpos=%04x -> WAIT_RASTER\n",
                    (unsigned)c->pc,
                    (unsigned)vhpos);
            c->state = COPPER_STATE_WAITING_RASTER;
        }
        return;
    }

    if (c->state == COPPER_STATE_WAITING_RASTER) {
        if (copper_wait_matches(c->waitpos, c->waitmask, vhpos)) {
            kprintf("[COPPER] raster wake pc=%05x vhpos=%04x\n",
                    (unsigned)c->pc,
                    (unsigned)vhpos);
            c->state = COPPER_STATE_FETCH_IR1;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void copper_init(CopperState *c)
{
    c->cop1lc = 0;
    c->cop2lc = 0;
    c->pc = 0;

    c->ir1 = 0;
    c->ir2 = 0;

    c->waitpos = 0;
    c->waitmask = 0;
    c->wait_bfd = 0;

    c->state = COPPER_STATE_HALTED;
    c->cdang = 0;
    c->after_vbl_reload = 0;
}

void copper_reset(CopperState *c)
{
    copper_init(c);
}

/* ------------------------------------------------------------------------- */
/* register access                                                           */
/* ------------------------------------------------------------------------- */

void copper_write_reg(CopperState *c, uint16_t reg, uint16_t value)
{
    switch (reg)
    {
    case COPPER_COP1LCH:
        c->cop1lc = (c->cop1lc & 0x0000FFFFu) | ((uint32_t)value << 16);
        kprintf("[COPPER] COP1LCH=%04x cop1lc=0x%06x\n",
                (unsigned)value,
                (unsigned)(c->cop1lc & CHIP_RAM_MASK));
        return;

    case COPPER_COP1LCL:
        c->cop1lc = (c->cop1lc & 0xFFFF0000u) | (uint32_t)(value & 0xFFFEu);
        kprintf("[COPPER] COP1LCL=%04x cop1lc=0x%06x\n",
                (unsigned)value,
                (unsigned)(c->cop1lc & CHIP_RAM_MASK));
        return;

    case COPPER_COP2LCH:
        c->cop2lc = (c->cop2lc & 0x0000FFFFu) | ((uint32_t)value << 16);
        kprintf("[COPPER] COP2LCH=%04x cop2lc=0x%06x\n",
                (unsigned)value,
                (unsigned)(c->cop2lc & CHIP_RAM_MASK));
        return;

    case COPPER_COP2LCL:
        c->cop2lc = (c->cop2lc & 0xFFFF0000u) | (uint32_t)(value & 0xFFFEu);
        kprintf("[COPPER] COP2LCL=%04x cop2lc=0x%06x\n",
                (unsigned)value,
                (unsigned)(c->cop2lc & CHIP_RAM_MASK));
        return;

    case COPPER_COPJMP1:
    {
        uint32_t old_pc = c->pc;
        c->pc = c->cop1lc & CHIP_RAM_MASK & ~1u;
        c->state = COPPER_STATE_FETCH_IR1;
        kprintf("[COPPER-JMP1] old_pc=%05x new_pc=%05x cop1lc=%05x\n",
                (unsigned)old_pc,
                (unsigned)c->pc,
                (unsigned)(c->cop1lc & CHIP_RAM_MASK));
        return;
    }

    case COPPER_COPJMP2:
    {
        uint32_t old_pc = c->pc;
        c->pc = c->cop2lc & CHIP_RAM_MASK & ~1u;
        c->state = COPPER_STATE_FETCH_IR1;
        kprintf("[COPPER-JMP2] old_pc=%05x new_pc=%05x cop2lc=%05x\n",
                (unsigned)old_pc,
                (unsigned)c->pc,
                (unsigned)(c->cop2lc & CHIP_RAM_MASK));
        return;
    }

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
    switch (reg)
    {
    case COPPER_COP1LCH: return (c->cop1lc >> 16) & 0x001Fu;
    case COPPER_COP1LCL: return c->cop1lc & 0xFFFEu;
    case COPPER_COP2LCH: return (c->cop2lc >> 16) & 0x001Fu;
    case COPPER_COP2LCL: return c->cop2lc & 0xFFFEu;
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------------- */
/* execution                                                                 */
/* ------------------------------------------------------------------------- */

void copper_vbl_reload(CopperState *c)
{
    c->pc = c->cop1lc & CHIP_RAM_MASK & ~1u;
    c->state = COPPER_STATE_FETCH_IR1;
    c->after_vbl_reload = 1;

    kprintf("[COPPER] vbl_reload cop1lc=0x%06x pc=0x%05x\n",
            (unsigned)(c->cop1lc & CHIP_RAM_MASK),
            (unsigned)c->pc);
}

void copper_blitter_done(CopperState *c)
{
    /*
     * Copper does not resume blindly; it only becomes runnable again.
     * Raster condition is still checked in copper_step().
     */
    if (c->state == COPPER_STATE_WAITING_BLITTER) {
        kprintf("[COPPER] blitter_done\n");
    }
}

void copper_step(CopperState *c, AgnusState *agnus, uint64_t ticks)
{
    uint64_t budget = ticks;

    while (budget > 0)
    {
        switch (c->state)
        {
        case COPPER_STATE_HALTED:
            return;

        case COPPER_STATE_WAITING_RASTER:
        case COPPER_STATE_WAITING_BLITTER:
            copper_poll_wait(c, agnus);
            return;

        case COPPER_STATE_FETCH_IR1:
            copper_fetch_ir1(c, agnus);
            budget--;
            break;

        case COPPER_STATE_FETCH_IR2_MOVE:
            copper_handle_move(c, agnus);
            budget--;
            break;

        case COPPER_STATE_FETCH_IR2_WAIT_SKIP:
            copper_handle_wait_skip(c, agnus);
            budget--;
            break;

        default:
            c->state = COPPER_STATE_HALTED;
            return;
        }
    }
}