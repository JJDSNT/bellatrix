#ifndef BELLATRIX_CHIPSET_CIA_H
#define BELLATRIX_CHIPSET_CIA_H

#include <stdint.h>
#include <stdbool.h>

struct Paula;

/* ------------------------------------------------------------------------- */
/* registers                                                                 */
/* ------------------------------------------------------------------------- */

enum {
    CIA_REG_PRA    = 0x0,
    CIA_REG_PRB    = 0x1,
    CIA_REG_DDRA   = 0x2,
    CIA_REG_DDRB   = 0x3,
    CIA_REG_TALO   = 0x4,
    CIA_REG_TAHI   = 0x5,
    CIA_REG_TBLO   = 0x6,
    CIA_REG_TBHI   = 0x7,
    CIA_REG_TODLO  = 0x8,
    CIA_REG_TODMID = 0x9,
    CIA_REG_TODHI  = 0xA,
    CIA_REG_UNUSED = 0xB,
    CIA_REG_SDR    = 0xC,
    CIA_REG_ICR    = 0xD,
    CIA_REG_CRA    = 0xE,
    CIA_REG_CRB    = 0xF,
};

/* ------------------------------------------------------------------------- */
/* interrupt bits                                                            */
/* ------------------------------------------------------------------------- */

#define CIA_ICR_TA      0x01u
#define CIA_ICR_TB      0x02u
#define CIA_ICR_ALRM    0x04u
#define CIA_ICR_SP      0x08u
#define CIA_ICR_FLG     0x10u
#define CIA_ICR_SETCLR  0x80u
#define CIA_ICR_IRQ     0x80u

/* ------------------------------------------------------------------------- */
/* control register A bits                                                   */
/* ------------------------------------------------------------------------- */

#define CIA_CRA_START    0x01u
#define CIA_CRA_PBON     0x02u
#define CIA_CRA_OUTMODE  0x04u
#define CIA_CRA_RUNMODE  0x08u
#define CIA_CRA_LOAD     0x10u
#define CIA_CRA_INMODE   0x20u
#define CIA_CRA_SPMODE   0x40u
#define CIA_CRA_TODIN    0x80u

/* ------------------------------------------------------------------------- */
/* control register B bits                                                   */
/* ------------------------------------------------------------------------- */

#define CIA_CRB_START     0x01u
#define CIA_CRB_PBON      0x02u
#define CIA_CRB_OUTMODE   0x04u
#define CIA_CRB_RUNMODE   0x08u
#define CIA_CRB_LOAD      0x10u
#define CIA_CRB_INMODE0   0x20u
#define CIA_CRB_INMODE1   0x40u
#define CIA_CRB_ALARM     0x80u

/* ------------------------------------------------------------------------- */
/* TOD timing (PAL model used by Bellatrix)                                  */
/* ------------------------------------------------------------------------- */

/*
 * CIA-A TOD: increment once per frame (VBL)
 */
#define CIA_A_TOD_TICKS_PER_INCREMENT (454u * 313u)

/*
 * CIA-B TOD: increment once per line (HSync)
 */
#define CIA_B_TOD_TICKS_PER_INCREMENT 454u

/* ------------------------------------------------------------------------- */
/* identity                                                                  */
/* ------------------------------------------------------------------------- */

typedef enum CIA_ID {
    CIA_PORT_A = 0,   /* raises PORTS (IPL 2) */
    CIA_PORT_B = 1    /* raises EXTER (IPL 6) */
} CIA_ID;

/* ------------------------------------------------------------------------- */
/* TOD state                                                                 */
/* ------------------------------------------------------------------------- */

typedef struct CIA_TOD_State {
    uint32_t counter;
    uint32_t alarm;

    uint32_t latch;
    bool     latched;

    uint32_t subticks;
    uint32_t ticks_per_inc;
} CIA_TOD_State;

/* ------------------------------------------------------------------------- */
/* CIA state                                                                 */
/* ------------------------------------------------------------------------- */

typedef struct CIA_State {
    CIA_ID id;

    /*
     * Parallel ports
     */
    uint8_t pra;
    uint8_t prb;
    uint8_t ddra;
    uint8_t ddrb;

    /*
     * External signals (injected by chipset glue)
     */
    uint8_t ext_pra;
    uint8_t ext_prb;

    /*
     * Serial
     */
    uint8_t sdr;

    /*
     * Interrupt
     */
    uint8_t icr_mask;
    uint8_t icr_data;

    /*
     * Control
     */
    uint8_t cra;
    uint8_t crb;

    /*
     * Timers
     */
    uint16_t ta_latch;
    uint16_t ta_counter;

    uint16_t tb_latch;
    uint16_t tb_counter;

    /*
     * TOD
     */
    CIA_TOD_State tod;

    /*
     * Paula wiring
     */
    uint8_t       irq_level;
    uint16_t      paula_irq_bit;
    struct Paula *paula;

    uint8_t irq_asserted;

} CIA_State;

typedef CIA_State CIA;

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void cia_init(CIA *cia, CIA_ID id);
void cia_reset(CIA *cia);

/* ------------------------------------------------------------------------- */
/* stepping                                                                  */
/* ------------------------------------------------------------------------- */

void cia_step(CIA *cia, uint64_t ticks);

/* ------------------------------------------------------------------------- */
/* wiring                                                                    */
/* ------------------------------------------------------------------------- */

void cia_attach_paula(CIA *cia, struct Paula *paula);

/* ------------------------------------------------------------------------- */
/* external pins                                                             */
/* ------------------------------------------------------------------------- */

void cia_set_external_pra(CIA *cia, uint8_t value);
void cia_set_external_prb(CIA *cia, uint8_t value);

/* ------------------------------------------------------------------------- */
/* IRQ                                                                       */
/* ------------------------------------------------------------------------- */

int     cia_irq_pending(const CIA *cia);
uint8_t cia_compute_ipl(const CIA *cia);

/* ------------------------------------------------------------------------- */
/* effective port values                                                     */
/* ------------------------------------------------------------------------- */

uint8_t cia_port_a_value(const CIA *cia);
uint8_t cia_port_b_value(const CIA *cia);

/* ------------------------------------------------------------------------- */
/* MMIO                                                                      */
/* ------------------------------------------------------------------------- */

uint8_t cia_read_reg(CIA *cia, uint8_t reg);
void    cia_write_reg(CIA *cia, uint8_t reg, uint8_t val);

/* ------------------------------------------------------------------------- */
/* TOD (implemented in cia_tod.c)                                            */
/* ------------------------------------------------------------------------- */

void    cia_tod_reset(CIA_TOD_State *tod, uint32_t ticks_per_inc);
void    cia_tod_step(CIA *cia, uint64_t ticks);
uint8_t cia_tod_read(CIA *cia, uint8_t reg);
void    cia_tod_write(CIA *cia, uint8_t reg, uint8_t val);

/* ------------------------------------------------------------------------- */
/* CIA A/B specializations                                                   */
/* ------------------------------------------------------------------------- */

void cia_a_apply_defaults(CIA *cia);
void cia_b_apply_defaults(CIA *cia);

#endif