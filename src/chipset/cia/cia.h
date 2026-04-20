#ifndef BELLATRIX_CHIPSET_CIA_H
#define BELLATRIX_CHIPSET_CIA_H

#include <stdint.h>
#include <stdbool.h>

struct Paula;

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

#define CIA_ICR_TA      0x01u
#define CIA_ICR_TB      0x02u
#define CIA_ICR_ALRM    0x04u
#define CIA_ICR_SP      0x08u
#define CIA_ICR_FLG     0x10u
#define CIA_ICR_SETCLR  0x80u
#define CIA_ICR_IRQ     0x80u

#define CIA_CRA_START    0x01u
#define CIA_CRA_PBON     0x02u
#define CIA_CRA_OUTMODE  0x04u
#define CIA_CRA_RUNMODE  0x08u
#define CIA_CRA_LOAD     0x10u
#define CIA_CRA_INMODE   0x20u
#define CIA_CRA_SPMODE   0x40u
#define CIA_CRA_TODIN    0x80u

#define CIA_CRB_START     0x01u
#define CIA_CRB_PBON      0x02u
#define CIA_CRB_OUTMODE   0x04u
#define CIA_CRB_RUNMODE   0x08u
#define CIA_CRB_LOAD      0x10u
#define CIA_CRB_INMODE0   0x20u
#define CIA_CRB_INMODE1   0x40u
#define CIA_CRB_ALARM     0x80u

#define CIA_TOD_TICKS_PER_INCREMENT 227u

typedef enum {
    CIA_PORT_A = 0,   /* CIA-A: raises PORTS (IPL 2) */
    CIA_PORT_B = 1    /* CIA-B: raises EXTER (IPL 6) */
} CIA_ID;

typedef struct CIA_State {
    uint8_t pra;
    uint8_t prb;
    uint8_t ddra;
    uint8_t ddrb;
    uint8_t sdr;

    uint8_t icr_mask;
    uint8_t icr_data;

    uint8_t cra;
    uint8_t crb;

    uint16_t ta_latch;
    uint16_t ta_counter;

    uint16_t tb_latch;
    uint16_t tb_counter;

    uint32_t tod;
    uint32_t tod_alarm;

    uint32_t tod_latch;
    bool     tod_latched;

    uint32_t tod_subticks;

    /* wiring */
    uint8_t       irq_level;     /* 2 = CIA-A (PORTS), 6 = CIA-B (EXTER)       */
    uint16_t      paula_irq_bit; /* PAULA_INT_PORTS or PAULA_INT_EXTER          */
    struct Paula *paula;         /* attached Paula; NULL until cia_attach_paula */
} CIA_State;

typedef CIA_State CIA;

void    cia_init(CIA *cia, CIA_ID id);
void    cia_reset(CIA *cia);
void    cia_step(CIA *cia, uint64_t ticks);
int     cia_irq_pending(const CIA *cia);
uint8_t cia_compute_ipl(const CIA *cia);
void    cia_attach_paula(CIA *cia, struct Paula *paula);

uint8_t cia_read_reg(CIA *cia, uint8_t reg);
void    cia_write_reg(CIA *cia, uint8_t reg, uint8_t val);

#endif
