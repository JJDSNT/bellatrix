// src/chipset/cia/cia.c

#include "cia.h"
#include "chipset/paula/paula.h"
#include "support.h"

static inline void cia_raise_icr(CIA *cia, uint8_t bits)
{
    cia->icr_data |= (uint8_t)(bits & 0x1Fu);

    if (cia_irq_pending(cia) && cia->paula)
        paula_irq_raise(cia->paula, cia->paula_irq_bit);
}

static int cia_timer_advance(uint16_t *counter,
                             uint16_t  latch,
                             bool      continuous,
                             uint64_t  ticks)
{
    if (ticks == 0)
        return 0;

    int underflows = 0;

    while (ticks >= (uint64_t)(*counter + 1u)) {
        ticks -= (uint64_t)(*counter + 1u);
        underflows++;

        if (!continuous) {
            *counter = 0;
            return underflows;
        }

        if (latch > 0)
            *counter = latch;
        else {
            *counter = 0;
            return underflows;
        }
    }

    *counter -= (uint16_t)ticks;
    return underflows;
}

static void cia_tod_increment(CIA *cia, uint32_t increments)
{
    while (increments--) {
        cia->tod = (cia->tod + 1u) & 0x00FFFFFFu;
        if (cia->tod == cia->tod_alarm) {
            kprintf("[CIA-B ALARM-HIT] tod=%06x alarm=%06x mask=%02x icr_before=%02x\n",
                    (unsigned)cia->tod, (unsigned)cia->tod_alarm,
                    (unsigned)cia->icr_mask, (unsigned)cia->icr_data);
            cia_raise_icr(cia, CIA_ICR_ALRM);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

void cia_init(CIA *cia, CIA_ID id)
{
    cia->pra  = 0xFF;
    cia->prb  = 0xFF;
    cia->ddra = 0x00;
    cia->ddrb = 0x00;
    cia->sdr  = 0x00;

    cia->icr_mask = 0x00;
    cia->icr_data = 0x00;

    cia->cra = 0x00;
    cia->crb = 0x00;

    cia->ta_latch   = 0xFFFF;
    cia->ta_counter = 0xFFFF;
    cia->tb_latch   = 0xFFFF;
    cia->tb_counter = 0xFFFF;

    cia->tod         = 0x000000u;
    cia->tod_alarm   = 0x00FFFFu;
    cia->tod_latch   = 0x000000u;
    cia->tod_latched = false;
    cia->tod_subticks = 0;

    cia->irq_level    = (id == CIA_PORT_A) ? 2u : 6u;
    cia->paula_irq_bit = (id == CIA_PORT_A) ? (uint16_t)PAULA_INT_PORTS
                                              : (uint16_t)PAULA_INT_EXTER;
    cia->tod_ticks_per_inc = (id == CIA_PORT_A) ? CIA_A_TOD_TICKS_PER_INCREMENT
                                                 : CIA_B_TOD_TICKS_PER_INCREMENT;
    cia->paula = NULL;
}

void cia_reset(CIA *cia)
{
    uint8_t       saved_irq_level    = cia->irq_level;
    uint16_t      saved_paula_irq    = cia->paula_irq_bit;
    uint32_t      saved_tod_rate     = cia->tod_ticks_per_inc;
    struct Paula *saved_paula        = cia->paula;

    cia->pra  = 0xFF;
    cia->prb  = 0xFF;
    cia->ddra = 0x00;
    cia->ddrb = 0x00;
    cia->sdr  = 0x00;

    cia->icr_mask = 0x00;
    cia->icr_data = 0x00;

    cia->cra = 0x00;
    cia->crb = 0x00;

    cia->ta_latch   = 0xFFFF;
    cia->ta_counter = 0xFFFF;
    cia->tb_latch   = 0xFFFF;
    cia->tb_counter = 0xFFFF;

    cia->tod         = 0x000000u;
    cia->tod_alarm   = 0x00FFFFu;
    cia->tod_latch   = 0x000000u;
    cia->tod_latched = false;
    cia->tod_subticks = 0;

    cia->irq_level        = saved_irq_level;
    cia->paula_irq_bit    = saved_paula_irq;
    cia->tod_ticks_per_inc = saved_tod_rate;
    cia->paula            = saved_paula;
}

/* ---------------------------------------------------------------------------
 * Wiring
 * ------------------------------------------------------------------------- */

void cia_attach_paula(CIA *cia, struct Paula *paula)
{
    cia->paula = paula;
}

/* ---------------------------------------------------------------------------
 * Step
 * ------------------------------------------------------------------------- */

void cia_step(CIA *cia, uint64_t ticks)
{
    if (!ticks)
        return;

    if ((cia->cra & CIA_CRA_START) && !(cia->cra & CIA_CRA_INMODE)) {
        bool continuous = !(cia->cra & CIA_CRA_RUNMODE);
        int uf = cia_timer_advance(&cia->ta_counter, cia->ta_latch, continuous, ticks);
        if (uf > 0) {
            cia_raise_icr(cia, CIA_ICR_TA);
            if (cia->cra & CIA_CRA_RUNMODE)
                cia->cra &= (uint8_t)~CIA_CRA_START;
        }
    }

    if (cia->crb & CIA_CRB_START) {
        uint8_t inmode = (uint8_t)((cia->crb >> 5) & 0x03u);
        if (inmode == 0) {
            bool continuous = !(cia->crb & CIA_CRB_RUNMODE);
            int uf = cia_timer_advance(&cia->tb_counter, cia->tb_latch, continuous, ticks);
            if (uf > 0) {
                cia_raise_icr(cia, CIA_ICR_TB);
                if (cia->crb & CIA_CRB_RUNMODE)
                    cia->crb &= (uint8_t)~CIA_CRB_START;
            }
        }
    }

    cia->tod_subticks += (uint32_t)ticks;
    if (cia->tod_subticks >= cia->tod_ticks_per_inc) {
        uint32_t inc = cia->tod_subticks / cia->tod_ticks_per_inc;
        cia->tod_subticks %= cia->tod_ticks_per_inc;
        cia_tod_increment(cia, inc);
    }
}

/* ---------------------------------------------------------------------------
 * IRQ status
 * ------------------------------------------------------------------------- */

int cia_irq_pending(const CIA *cia)
{
    return (cia->icr_data & cia->icr_mask) != 0;
}

uint8_t cia_compute_ipl(const CIA *cia)
{
    return cia_irq_pending(cia) ? cia->irq_level : 0u;
}

/* ---------------------------------------------------------------------------
 * MMIO
 * ------------------------------------------------------------------------- */

uint8_t cia_read_reg(CIA *cia, uint8_t reg)
{
    switch (reg) {
    case CIA_REG_PRA:
        return (uint8_t)((cia->pra & cia->ddra) | (~cia->ddra & 0xFFu));

    case CIA_REG_PRB:
        return (uint8_t)((cia->prb & cia->ddrb) | (~cia->ddrb & 0xFFu));

    case CIA_REG_DDRA:
        return cia->ddra;

    case CIA_REG_DDRB:
        return cia->ddrb;

    case CIA_REG_TALO:
        return (uint8_t)(cia->ta_counter & 0x00FFu);

    case CIA_REG_TAHI:
        return (uint8_t)((cia->ta_counter >> 8) & 0x00FFu);

    case CIA_REG_TBLO:
        return (uint8_t)(cia->tb_counter & 0x00FFu);

    case CIA_REG_TBHI:
        return (uint8_t)((cia->tb_counter >> 8) & 0x00FFu);

    case CIA_REG_TODLO:
        if (cia->tod_latched) {
            uint8_t v = (uint8_t)(cia->tod_latch & 0xFFu);
            cia->tod_latched = false;
            return v;
        }
        return (uint8_t)(cia->tod & 0xFFu);

    case CIA_REG_TODMID:
        if (cia->tod_latched)
            return (uint8_t)((cia->tod_latch >> 8) & 0xFFu);
        return (uint8_t)((cia->tod >> 8) & 0xFFu);

    case CIA_REG_TODHI:
        cia->tod_latch   = cia->tod;
        cia->tod_latched = true;
        return (uint8_t)((cia->tod >> 16) & 0xFFu);

    case CIA_REG_SDR:
        return cia->sdr;

    case CIA_REG_ICR: {
        uint8_t val = cia->icr_data;
        if (cia->icr_data & cia->icr_mask)
            val |= CIA_ICR_IRQ;
        kprintf("[CIA] ICR read -> %02x (mask=%02x data=%02x), clearing latched data\n",
                (unsigned)val, (unsigned)cia->icr_mask, (unsigned)cia->icr_data);
        cia->icr_data = 0x00;
        /* Clear Paula bit since CPU acknowledged */
        if (cia->paula)
            paula_irq_clear(cia->paula, cia->paula_irq_bit);
        return val;
    }

    case CIA_REG_CRA:
        return cia->cra;

    case CIA_REG_CRB:
        return cia->crb;

    default:
        kprintf("[CIA] unhandled read reg=%02x\n", (unsigned)reg);
        return 0xFF;
    }
}

void cia_write_reg(CIA *cia, uint8_t reg, uint8_t val)
{
    switch (reg) {
    case CIA_REG_PRA:   cia->pra  = val; return;
    case CIA_REG_PRB:   cia->prb  = val; return;
    case CIA_REG_DDRA:  cia->ddra = val; return;
    case CIA_REG_DDRB:  cia->ddrb = val; return;

    case CIA_REG_TALO:
        cia->ta_latch = (uint16_t)((cia->ta_latch & 0xFF00u) | val);
        return;

    case CIA_REG_TAHI:
        cia->ta_latch = (uint16_t)((cia->ta_latch & 0x00FFu) | ((uint16_t)val << 8));
        if (!(cia->cra & CIA_CRA_START))
            cia->ta_counter = cia->ta_latch;
        return;

    case CIA_REG_TBLO:
        cia->tb_latch = (uint16_t)((cia->tb_latch & 0xFF00u) | val);
        return;

    case CIA_REG_TBHI:
        cia->tb_latch = (uint16_t)((cia->tb_latch & 0x00FFu) | ((uint16_t)val << 8));
        if (!(cia->crb & CIA_CRB_START))
            cia->tb_counter = cia->tb_latch;
        return;

    case CIA_REG_TODLO:
        if (cia->crb & CIA_CRB_ALARM) {
            cia->tod_alarm = (cia->tod_alarm & 0xFFFF00u) | (uint32_t)val;
            kprintf("[CIA-B ALARM-WRITE] TODLO=%02x -> alarm=%06x tod=%06x\n",
                    (unsigned)val, (unsigned)cia->tod_alarm, (unsigned)cia->tod);
        } else {
            cia->tod = (cia->tod & 0xFFFF00u) | (uint32_t)val;
        }
        return;

    case CIA_REG_TODMID:
        if (cia->crb & CIA_CRB_ALARM) {
            cia->tod_alarm = (cia->tod_alarm & 0xFF00FFu) | ((uint32_t)val << 8);
            kprintf("[CIA-B ALARM-WRITE] TODMID=%02x -> alarm=%06x tod=%06x\n",
                    (unsigned)val, (unsigned)cia->tod_alarm, (unsigned)cia->tod);
        } else {
            cia->tod = (cia->tod & 0xFF00FFu) | ((uint32_t)val << 8);
        }
        return;

    case CIA_REG_TODHI:
        if (cia->crb & CIA_CRB_ALARM) {
            cia->tod_alarm = (cia->tod_alarm & 0x00FFFFu) | ((uint32_t)val << 16);
            cia->tod_alarm &= 0x00FFFFFFu;
            kprintf("[CIA-B ALARM-WRITE] TODHI=%02x -> alarm=%06x tod=%06x\n",
                    (unsigned)val, (unsigned)cia->tod_alarm, (unsigned)cia->tod);
        } else {
            cia->tod = (cia->tod & 0x00FFFFu) | ((uint32_t)val << 16);
            cia->tod &= 0x00FFFFFFu;
        }
        return;

    case CIA_REG_SDR:
        cia->sdr = val;
        return;

    case CIA_REG_ICR:
        if (val & CIA_ICR_SETCLR)
            cia->icr_mask |= (uint8_t)(val & 0x1Fu);
        else
            cia->icr_mask &= (uint8_t)~(val & 0x1Fu);
        kprintf("[CIA] ICR write val=%02x -> mask=%02x\n",
                (unsigned)val, (unsigned)cia->icr_mask);
        return;

    case CIA_REG_CRA:
        cia->cra = (uint8_t)(val & (uint8_t)~CIA_CRA_LOAD);
        if (val & CIA_CRA_LOAD)
            cia->ta_counter = cia->ta_latch;
        kprintf("[CIA] CRA <- %02x  counter=%04x latch=%04x\n",
                (unsigned)cia->cra, (unsigned)cia->ta_counter, (unsigned)cia->ta_latch);
        return;

    case CIA_REG_CRB:
        cia->crb = (uint8_t)(val & (uint8_t)~CIA_CRB_LOAD);
        if (val & CIA_CRB_LOAD)
            cia->tb_counter = cia->tb_latch;
        kprintf("[CIA] CRB <- %02x  counter=%04x latch=%04x alarm_mode=%d\n",
                (unsigned)cia->crb, (unsigned)cia->tb_counter,
                (unsigned)cia->tb_latch, !!(cia->crb & CIA_CRB_ALARM));
        return;

    default:
        kprintf("[CIA] unhandled write reg=%02x val=%02x\n", (unsigned)reg, (unsigned)val);
        return;
    }
}
