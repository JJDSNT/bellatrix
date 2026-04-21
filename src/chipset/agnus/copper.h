#ifndef BELLATRIX_CHIPSET_AGNUS_COPPER_H
#define BELLATRIX_CHIPSET_AGNUS_COPPER_H

#include <stdint.h>

struct AgnusState;

/* ------------------------------------------------------------------------- */
/* Register offsets                                                          */
/* ------------------------------------------------------------------------- */

#define COPPER_COP1LCH 0x0080u
#define COPPER_COP1LCL 0x0082u
#define COPPER_COP2LCH 0x0084u
#define COPPER_COP2LCL 0x0086u
#define COPPER_COPJMP1 0x0088u
#define COPPER_COPJMP2 0x008Au
#define COPPER_COPINS  0x008Cu

/* ------------------------------------------------------------------------- */
/* Internal state machine                                                    */
/* ------------------------------------------------------------------------- */

typedef enum CopperExecState
{
    COPPER_STATE_FETCH_IR1 = 0,
    COPPER_STATE_FETCH_IR2_WAIT_SKIP,
    COPPER_STATE_FETCH_IR2_MOVE,
    COPPER_STATE_WAITING_RASTER,
    COPPER_STATE_WAITING_BLITTER,
    COPPER_STATE_HALTED

} CopperExecState;

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

typedef struct CopperState
{
    /*
     * List pointers
     */
    uint32_t cop1lc;
    uint32_t cop2lc;

    /*
     * Program counter
     */
    uint32_t pc;

    /*
     * Instruction registers
     */
    uint16_t ir1;
    uint16_t ir2;

    /*
     * WAIT state
     */
    uint16_t waitpos;
    uint16_t waitmask;
    uint8_t  wait_bfd;   /* wait for blitter done */

    /*
     * Execution state
     */
    uint8_t state;

    /*
     * COPCON (CDANG)
     */
    uint8_t cdang;

    /*
     * Debug / bring-up helper
     */
    uint8_t after_vbl_reload;

} CopperState;

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void copper_init(CopperState *c);
void copper_reset(CopperState *c);

/* ------------------------------------------------------------------------- */
/* MMIO                                                                      */
/* ------------------------------------------------------------------------- */

void     copper_write_reg(CopperState *c, uint16_t reg, uint16_t value);
uint32_t copper_read_reg(CopperState *c, uint16_t reg);

/* ------------------------------------------------------------------------- */
/* Execution                                                                 */
/* ------------------------------------------------------------------------- */

void copper_vbl_reload(CopperState *c);

/*
 * Called by blitter when it finishes (for BFD WAIT)
 */
void copper_blitter_done(CopperState *c);

/*
 * Step Copper execution by 'ticks'
 */
void copper_step(CopperState *c, struct AgnusState *agnus, uint64_t ticks);

#endif