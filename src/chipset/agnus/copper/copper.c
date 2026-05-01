// src/chipset/agnus/copper/copper.c

//
// Copper core — instruction execution only.
// Timing / scheduling is handled by copper_service.
//

#include "copper.h"

#include "../agnus.h"
#include "../beam.h"
#include "../blitter.h"
#include "memory/memory.h"
#include "support.h"

/* ------------------------------------------------------------------------- */
/* constants                                                                 */
/* ------------------------------------------------------------------------- */

#define CHIP_RAM_MASK 0x001FFFFFu

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

static inline uint16_t rd16(const AgnusState *agnus, uint32_t addr)
{
    return bellatrix_chip_read16(agnus->memory, addr & CHIP_RAM_MASK);
}

static inline uint16_t vhpos(const AgnusState *agnus)
{
    return (uint16_t)(((agnus->beam.vpos & 0xFFu) << 8) |
                      (agnus->beam.hpos & 0xFEu));
}

static inline void clear_transient_state(CopperState *c)
{
    c->ir1 = 0;
    c->ir2 = 0;
    c->waitpos = 0;
    c->waitmask = 0;
    c->wait_bfd = 0;
    c->after_vbl_reload = 0;
}

/* ------------------------------------------------------------------------- */
/* decoding                                                                  */
/* ------------------------------------------------------------------------- */

static inline int is_move(uint16_t ir1) { return !(ir1 & 1); }
static inline int is_wait(uint16_t ir1, uint16_t ir2) { return (ir1 & 1) && !(ir2 & 1); }
static inline int is_skip(uint16_t ir1, uint16_t ir2) { return (ir1 & 1) && (ir2 & 1); }

static inline uint16_t ra(uint16_t ir1) { return ir1 & 0x01FE; }
static inline uint16_t dw(uint16_t ir2) { return ir2; }

static inline uint16_t waitpos(uint16_t ir1) { return ir1 & 0xFFFE; }

static inline uint16_t waitmask(uint16_t ir2)
{
    return (ir2 & 0x7FFE) | 0x8001;
}

static inline int wait_bfd(uint16_t ir2)
{
    return (ir2 & 0x8000) ? 1 : 0;
}

static inline int copper_watch_reg(uint16_t addr)
{
    switch (addr)
    {
    case 0x0080u:
    case 0x0082u:
    case 0x0084u:
    case 0x0086u:
    case 0x0088u:
    case 0x008Au:
    case 0x008Eu:
    case 0x0090u:
    case 0x0092u:
    case 0x0094u:
    case 0x00E0u:
    case 0x00E2u:
    case 0x00E4u:
    case 0x00E6u:
    case 0x0100u:
    case 0x0180u:
        return 1;
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------------- */
/* wait logic                                                                */
/* ------------------------------------------------------------------------- */

static inline int wait_match(uint16_t wp, uint16_t wm, uint16_t vh)
{
    uint8_t vmask = (wm >> 8) & 0xFF;
    uint8_t vwait = (wp >> 8) & 0xFF;
    uint8_t vcur  = (vh >> 8) & 0xFF;

    if ((vcur & vmask) > (vwait & vmask)) return 1;
    if ((vcur & vmask) < (vwait & vmask)) return 0;

    uint8_t hmask = wm & 0xFE;
    uint8_t hwait = wp & 0xFE;
    uint8_t hcur  = vh & 0xFE;

    uint8_t hcmp = (hcur < 0xE0) ? ((hcur + 2) & 0xFE)
                                : ((hcur - 0xE0) & 0xFE);

    return ((hcmp & hmask) >= (hwait & hmask));
}

/* ------------------------------------------------------------------------- */
/* state machine                                                             */
/* ------------------------------------------------------------------------- */

static void fetch_ir1(CopperState *c, const AgnusState *agnus)
{
    c->ir1 = rd16(agnus, c->pc);
    c->pc = (c->pc + 2) & CHIP_RAM_MASK;

    c->state = is_move(c->ir1)
        ? COPPER_STATE_FETCH_IR2_MOVE
        : COPPER_STATE_FETCH_IR2_WAIT_SKIP;
}

static void exec_move(CopperState *c, AgnusState *agnus)
{
    uint16_t addr = ra(c->ir1);

    c->ir2 = rd16(agnus, c->pc);
    c->pc = (c->pc + 2) & CHIP_RAM_MASK;

    /* illegal guard */

    if ((addr & 1) || addr > 0x01FE ||
        (!c->cdang && addr < 0x80) ||
        ( c->cdang && addr < 0x40))
    {
        kprintf("[COPPER-HALT] illegal MOVE %04x\n", addr);
        c->state = COPPER_STATE_HALTED;
        return;
    }

    if (copper_watch_reg(addr))
    {
        kprintf("[COPPER-MOVE] pc=%05x vh=%04x addr=%04x <- %04x\n",
                (unsigned)((c->pc - 4u) & CHIP_RAM_MASK),
                (unsigned)vhpos(agnus),
                (unsigned)addr,
                (unsigned)dw(c->ir2));
    }

    agnus_write_reg(agnus, addr, dw(c->ir2), 2);
    c->state = COPPER_STATE_FETCH_IR1;
}

static void exec_wait_skip(CopperState *c, AgnusState *agnus)
{
    c->ir2 = rd16(agnus, c->pc);
    c->pc = (c->pc + 2) & CHIP_RAM_MASK;

    if (c->ir1 == 0xFFFF && (c->ir2 & 0xFFFE) == 0xFFFE)
    {
        c->state = COPPER_STATE_HALTED;
        return;
    }

    c->waitpos  = waitpos(c->ir1);
    c->waitmask = waitmask(c->ir2);
    c->wait_bfd = wait_bfd(c->ir2);

    uint16_t vh = vhpos(agnus);

    if (is_skip(c->ir1, c->ir2))
    {
        if (wait_match(c->waitpos, c->waitmask, vh))
            c->pc = (c->pc + 4) & CHIP_RAM_MASK;

        c->state = COPPER_STATE_FETCH_IR1;
        return;
    }

    if (is_wait(c->ir1, c->ir2))
    {
        if (((c->ir1 & 0xFF00u) == 0xFF00u) || ((c->waitpos & 0xFF00u) == 0xFF00u))
        {
            kprintf("[COPPER-WAIT-HI] pc=%05x ir1=%04x ir2=%04x vh=%04x mask=%04x\n",
                    (unsigned)((c->pc - 4u) & CHIP_RAM_MASK),
                    (unsigned)c->ir1,
                    (unsigned)c->ir2,
                    (unsigned)vh,
                    (unsigned)c->waitmask);
        }

        if (!c->wait_bfd && blitter_is_busy(&agnus->blitter))
        {
            c->state = COPPER_STATE_WAITING_BLITTER;
            return;
        }

        if (wait_match(c->waitpos, c->waitmask, vh))
        {
            c->state = COPPER_STATE_FETCH_IR1;
            return;
        }

        c->state = COPPER_STATE_WAITING_RASTER;
        return;
    }

    c->state = COPPER_STATE_FETCH_IR1;
}

/* ------------------------------------------------------------------------- */
/* public API                                                                */
/* ------------------------------------------------------------------------- */

void copper_init(CopperState *c)
{
    c->cop1lc = 0;
    c->cop2lc = 0;
    c->pc     = 0;

    clear_transient_state(c);

    c->state = COPPER_STATE_HALTED;
    c->cdang = 0;
}

void copper_reset(CopperState *c)
{
    copper_init(c);
}

void copper_restart_program(CopperState *c, uint32_t new_pc)
{
    clear_transient_state(c);
    c->pc = new_pc & CHIP_RAM_MASK & ~1u;
    c->state = COPPER_STATE_FETCH_IR1;
}

void copper_step_exec(CopperState *c, AgnusState *agnus, int cycles)
{
    while (cycles-- > 0)
    {
        switch (c->state)
        {
        case COPPER_STATE_HALTED:
            return;

        case COPPER_STATE_WAITING_RASTER:
        case COPPER_STATE_WAITING_BLITTER:
            return;

        case COPPER_STATE_FETCH_IR1:
            fetch_ir1(c, agnus);
            break;

        case COPPER_STATE_FETCH_IR2_MOVE:
            exec_move(c, agnus);
            break;

        case COPPER_STATE_FETCH_IR2_WAIT_SKIP:
            exec_wait_skip(c, agnus);
            break;
        }
    }
}
