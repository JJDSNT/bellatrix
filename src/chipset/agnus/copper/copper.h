// src/chipset/agnus/copper/copper.h

#pragma once

#include <stdint.h>

/* forward declarations */

typedef struct AgnusState AgnusState;

/* ------------------------------------------------------------------------- */
/* custom register offsets                                                   */
/* ------------------------------------------------------------------------- */

#define COPPER_COP1LCH 0x0080u
#define COPPER_COP1LCL 0x0082u
#define COPPER_COP2LCH 0x0084u
#define COPPER_COP2LCL 0x0086u
#define COPPER_COPJMP1 0x0088u
#define COPPER_COPJMP2 0x008Au
#define COPPER_COPINS  0x008Cu

#define AGNUS_COPCON   0x002Eu

/* ------------------------------------------------------------------------- */
/* copper execution state                                                    */
/* ------------------------------------------------------------------------- */

typedef enum CopperExecState
{
    COPPER_STATE_HALTED = 0,
    COPPER_STATE_FETCH_IR1,
    COPPER_STATE_FETCH_IR2_MOVE,
    COPPER_STATE_FETCH_IR2_WAIT_SKIP,
    COPPER_STATE_WAITING_RASTER,
    COPPER_STATE_WAITING_BLITTER

} CopperExecState;

/* ------------------------------------------------------------------------- */
/* copper state                                                              */
/* ------------------------------------------------------------------------- */

typedef struct CopperState
{
    uint32_t cop1lc;
    uint32_t cop2lc;
    uint32_t pc;

    uint16_t ir1;
    uint16_t ir2;

    uint16_t waitpos;
    uint16_t waitmask;
    uint8_t  wait_bfd;

    CopperExecState state;

    uint8_t cdang;
    uint8_t after_vbl_reload;

} CopperState;

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void copper_init(CopperState *c);
void copper_reset(CopperState *c);
void copper_restart_program(CopperState *c,
                            uint32_t new_pc);

/* ------------------------------------------------------------------------- */
/* execution core                                                            */
/* ------------------------------------------------------------------------- */

void copper_step_exec(CopperState *c,
                      AgnusState *agnus,
                      int cycles);
