// src/chipset/cia/cia.c

#include "cia.h"
#include "chipset/paula/paula.h"
#include "support.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* internal helpers                                                          */
/* ------------------------------------------------------------------------- */

static inline void cia_sync_irq_line(CIA *cia)
{
    if (!cia->paula)
        return;

    if (cia_irq_pending(cia)) {
        paula_irq_raise(cia->paula, cia->paula_irq_bit);
        if (!cia->irq_asserted) {
            kprintf("[CIA%c-IRQ] raised data=%02x mask=%02x\n",
                    cia->id == CIA_PORT_A ? 'A' : 'B',
                    (unsigned)cia->icr_data,
                    (unsigned)cia->icr_mask);
        }
        cia->irq_asserted = 1u;
    } else {
        paula_irq_clear(cia->paula, cia->paula_irq_bit);
        if (cia->irq_asserted) {
            kprintf("[CIA%c-IRQ] cleared\n",
                    cia->id == CIA_PORT_A ? 'A' : 'B');
        }
        cia->irq_asserted = 0u;
    }
}

static inline void cia_raise_icr(CIA *cia, uint8_t bits)
{
    cia->icr_data |= (uint8_t)(bits & 0x1Fu);
    cia_sync_irq_line(cia);
}

static inline void cia_reset_core_state(CIA *cia)
{
    cia->pra  = 0xFFu;
    cia->prb  = 0xFFu;
    cia->ddra = 0x00u;
    cia->ddrb = 0x00u;

    cia->ext_pra = 0xFFu;
    cia->ext_prb = 0xFFu;

    cia->sdr = 0x00u;

    cia->icr_mask = 0x00u;
    cia->icr_data = 0x00u;

    cia->cra = 0x00u;
    cia->crb = 0x00u;

    cia->ta_latch   = 0xFFFFu;
    cia->ta_counter = 0xFFFFu;
    cia->tb_latch   = 0xFFFFu;
    cia->tb_counter = 0xFFFFu;

    cia->irq_asserted = 0u;
}

static int cia_timer_advance(uint16_t *counter,
                             uint16_t  latch,
                             bool      continuous,
                             uint64_t  ticks)
{
    int underflows = 0;

    if (ticks == 0)
        return 0;

    while (ticks >= (uint64_t)(*counter + 1u)) {
        ticks -= (uint64_t)(*counter + 1u);
        underflows++;

        if (!continuous) {
            *counter = 0;
            return underflows;
        }

        if (latch > 0) {
            *counter = latch;
        } else {
            *counter = 0;
            return underflows;
        }
    }

    *counter -= (uint16_t)ticks;
    return underflows;
}

static int cia_timer_advance_events(uint16_t *counter,
                                    uint16_t  latch,
                                    bool      continuous,
                                    uint32_t  events)
{
    int underflows = 0;

    while (events > 0) {
        if (*counter == 0) {
            underflows++;

            if (!continuous)
                return underflows;

            if (latch == 0)
                return underflows;

            *counter = latch;
        }

        if (events > (uint32_t)(*counter + 1u)) {
            events -= (uint32_t)(*counter + 1u);
            underflows++;

            if (!continuous) {
                *counter = 0;
                return underflows;
            }

            if (latch == 0) {
                *counter = 0;
                return underflows;
            }

            *counter = latch;
        } else {
            *counter = (uint16_t)(*counter - events);
            events = 0;
        }
    }

    return underflows;
}

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void cia_init(CIA *cia, CIA_ID id)
{
    memset(cia, 0, sizeof(*cia));

    cia->id = id;
    cia->irq_level     = (id == CIA_PORT_A) ? 2u : 6u;
    cia->paula_irq_bit = (id == CIA_PORT_A) ? (uint16_t)PAULA_INT_PORTS
                                            : (uint16_t)PAULA_INT_EXTER;
    cia->paula = NULL;

    cia_reset(cia);
}

void cia_reset(CIA *cia)
{
    CIA_ID         saved_id       = cia->id;
    uint8_t        saved_irq      = cia->irq_level;
    uint16_t       saved_paulabit = cia->paula_irq_bit;
    struct Paula  *saved_paula    = cia->paula;

    cia_reset_core_state(cia);

    cia->id            = saved_id;
    cia->irq_level     = saved_irq;
    cia->paula_irq_bit = saved_paulabit;
    cia->paula         = saved_paula;

    cia_tod_reset(&cia->tod,
                  (cia->id == CIA_PORT_A) ?
                    CIA_A_TOD_TICKS_PER_INCREMENT :
                    CIA_B_TOD_TICKS_PER_INCREMENT);

    if (cia->id == CIA_PORT_A)
        cia_a_apply_defaults(cia);
    else
        cia_b_apply_defaults(cia);

    cia_sync_irq_line(cia);
}

/* ------------------------------------------------------------------------- */
/* wiring                                                                    */
/* ------------------------------------------------------------------------- */

void cia_attach_paula(CIA *cia, struct Paula *paula)
{
    cia->paula = paula;
    cia_sync_irq_line(cia);
}

/* ------------------------------------------------------------------------- */
/* external pins                                                             */
/* ------------------------------------------------------------------------- */

void cia_set_external_pra(CIA *cia, uint8_t value)
{
    cia->ext_pra = value;
}

void cia_set_external_prb(CIA *cia, uint8_t value)
{
    cia->ext_prb = value;
}

/* ------------------------------------------------------------------------- */
/* irq                                                                       */
/* ------------------------------------------------------------------------- */

int cia_irq_pending(const CIA *cia)
{
    return (cia->icr_data & cia->icr_mask) != 0;
}

uint8_t cia_compute_ipl(const CIA *cia)
{
    return cia_irq_pending(cia) ? cia->irq_level : 0u;
}

/* ------------------------------------------------------------------------- */
/* effective port values                                                     */
/* ------------------------------------------------------------------------- */

uint8_t cia_port_a_value(const CIA *cia)
{
    return (uint8_t)((cia->pra & cia->ddra) | (cia->ext_pra & (uint8_t)~cia->ddra));
}

uint8_t cia_port_b_value(const CIA *cia)
{
    return (uint8_t)((cia->prb & cia->ddrb) | (cia->ext_prb & (uint8_t)~cia->ddrb));
}

/* ------------------------------------------------------------------------- */
/* stepping                                                                  */
/* ------------------------------------------------------------------------- */

void cia_step(CIA *cia, uint64_t ticks)
{
    int uf_a = 0;
    int uf_b = 0;

    if (ticks == 0)
        return;

    /*
     * Timer A
     *
     * Current Bellatrix stage:
     *   - Phi2 clock mode implemented
     *   - CNT mode not implemented yet
     */
    if ((cia->cra & CIA_CRA_START) && !(cia->cra & CIA_CRA_INMODE)) {
        bool continuous = (cia->cra & CIA_CRA_RUNMODE) ? false : true;

        uf_a = cia_timer_advance(&cia->ta_counter,
                                 cia->ta_latch,
                                 continuous,
                                 ticks);
        if (uf_a > 0) {
            cia_raise_icr(cia, CIA_ICR_TA);

            if (cia->cra & CIA_CRA_RUNMODE)
                cia->cra &= (uint8_t)~CIA_CRA_START;
        }
    }

    /*
     * Timer B
     *
     * Modes:
     *   00 = Phi2
     *   01 = CNT
     *   10 = Timer A underflow
     *   11 = Timer A underflow + CNT
     *
     * Current Bellatrix stage:
     *   - 00 implemented
     *   - 10 implemented
     *   - 01 and 11 left inert for now
     */
    if (cia->crb & CIA_CRB_START) {
        uint8_t inmode = (uint8_t)((cia->crb >> 5) & 0x03u);
        bool continuous = (cia->crb & CIA_CRB_RUNMODE) ? false : true;

        if (inmode == 0u) {
            uf_b = cia_timer_advance(&cia->tb_counter,
                                     cia->tb_latch,
                                     continuous,
                                     ticks);
        } else if (inmode == 2u && uf_a > 0) {
            uf_b = cia_timer_advance_events(&cia->tb_counter,
                                            cia->tb_latch,
                                            continuous,
                                            (uint32_t)uf_a);
        }

        if (uf_b > 0) {
            cia_raise_icr(cia, CIA_ICR_TB);

            if (cia->crb & CIA_CRB_RUNMODE)
                cia->crb &= (uint8_t)~CIA_CRB_START;
        }
    }

    cia_tod_step(cia, ticks);
}

/* ------------------------------------------------------------------------- */
/* mmio                                                                      */
/* ------------------------------------------------------------------------- */

uint8_t cia_read_reg(CIA *cia, uint8_t reg)
{
    switch (reg & 0x0Fu) {

        case CIA_REG_PRA: {
            uint8_t v = cia_port_a_value(cia);
            /* log only on value change to avoid flooding tight polling loops */
            static uint8_t last_pra_a, last_pra_b;
            uint8_t *last = (cia->id == CIA_PORT_A) ? &last_pra_a : &last_pra_b;
            if (v != *last) {
                kprintf("[CIA%c-PRA-R] -> %02x  (pra=%02x ddra=%02x ext=%02x)\n",
                        cia->id == CIA_PORT_A ? 'A' : 'B',
                        (unsigned)v, (unsigned)cia->pra,
                        (unsigned)cia->ddra, (unsigned)cia->ext_pra);
                *last = v;
            }
            return v;
        }

        case CIA_REG_PRB: {
            uint8_t v = cia_port_b_value(cia);
            static uint8_t last_prb_a, last_prb_b;
            uint8_t *last = (cia->id == CIA_PORT_A) ? &last_prb_a : &last_prb_b;
            if (v != *last) {
                kprintf("[CIA%c-PRB-R] -> %02x  (prb=%02x ddrb=%02x ext=%02x)\n",
                        cia->id == CIA_PORT_A ? 'A' : 'B',
                        (unsigned)v, (unsigned)cia->prb,
                        (unsigned)cia->ddrb, (unsigned)cia->ext_prb);
                *last = v;
            }
            return v;
        }

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
        case CIA_REG_TODMID:
        case CIA_REG_TODHI:
            return cia_tod_read(cia, reg & 0x0Fu);

        case CIA_REG_UNUSED:
            return 0xFFu;

        case CIA_REG_SDR:
            return cia->sdr;

        case CIA_REG_ICR: {
            uint8_t val = cia->icr_data;

            if (cia_irq_pending(cia))
                val |= CIA_ICR_IRQ;

            cia->icr_data = 0x00u;
            cia_sync_irq_line(cia);

            kprintf("[CIA%c-ICR-R] returned=%02x (mask=%02x)\n",
                    cia->id == CIA_PORT_A ? 'A' : 'B',
                    (unsigned)val,
                    (unsigned)cia->icr_mask);
            return val;
        }

        case CIA_REG_CRA:
            return cia->cra;

        case CIA_REG_CRB:
            return cia->crb;

        default:
            return 0xFFu;
    }
}

void cia_write_reg(CIA *cia, uint8_t reg, uint8_t val)
{
    switch (reg & 0x0Fu) {

        case CIA_REG_PRA:
            kprintf("[CIA%c-PRA-W] old=%02x new=%02x ddra=%02x ext=%02x result=%02x\n",
                    cia->id == CIA_PORT_A ? 'A' : 'B',
                    (unsigned)cia->pra, (unsigned)val,
                    (unsigned)cia->ddra, (unsigned)cia->ext_pra,
                    (unsigned)((val & cia->ddra) | (cia->ext_pra & (uint8_t)~cia->ddra)));
            cia->pra = val;
            return;

        case CIA_REG_PRB:
            kprintf("[CIA%c-PRB-W] old=%02x new=%02x ddrb=%02x ext=%02x result=%02x\n",
                    cia->id == CIA_PORT_A ? 'A' : 'B',
                    (unsigned)cia->prb, (unsigned)val,
                    (unsigned)cia->ddrb, (unsigned)cia->ext_prb,
                    (unsigned)((val & cia->ddrb) | (cia->ext_prb & (uint8_t)~cia->ddrb)));
            cia->prb = val;
            return;

        case CIA_REG_DDRA:
            kprintf("[CIA%c-DDRA-W] old=%02x new=%02x\n",
                    cia->id == CIA_PORT_A ? 'A' : 'B',
                    (unsigned)cia->ddra, (unsigned)val);
            cia->ddra = val;
            return;

        case CIA_REG_DDRB:
            kprintf("[CIA%c-DDRB-W] old=%02x new=%02x\n",
                    cia->id == CIA_PORT_A ? 'A' : 'B',
                    (unsigned)cia->ddrb, (unsigned)val);
            cia->ddrb = val;
            return;

        case CIA_REG_TALO:
            cia->ta_latch = (uint16_t)((cia->ta_latch & 0xFF00u) | (uint16_t)val);
            return;

        case CIA_REG_TAHI:
            cia->ta_latch = (uint16_t)((cia->ta_latch & 0x00FFu) | ((uint16_t)val << 8));
            if (!(cia->cra & CIA_CRA_START))
                cia->ta_counter = cia->ta_latch;
            return;

        case CIA_REG_TBLO:
            cia->tb_latch = (uint16_t)((cia->tb_latch & 0xFF00u) | (uint16_t)val);
            return;

        case CIA_REG_TBHI:
            cia->tb_latch = (uint16_t)((cia->tb_latch & 0x00FFu) | ((uint16_t)val << 8));
            if (!(cia->crb & CIA_CRB_START))
                cia->tb_counter = cia->tb_latch;
            return;

        case CIA_REG_TODLO:
        case CIA_REG_TODMID:
        case CIA_REG_TODHI:
            cia_tod_write(cia, reg & 0x0Fu, val);
            return;

        case CIA_REG_UNUSED:
            return;

        case CIA_REG_SDR:
            cia->sdr = val;
            /* In output mode (CRA bit 6), writing SDR starts a shift; fire SP immediately */
            if (cia->cra & CIA_CRA_SPMODE)
                cia_raise_icr(cia, CIA_ICR_SP);
            return;

        case CIA_REG_ICR:
            if (val & CIA_ICR_SETCLR)
                cia->icr_mask |= (uint8_t)(val & 0x1Fu);
            else
                cia->icr_mask &= (uint8_t)~(val & 0x1Fu);

            kprintf("[CIA%c-ICR-W] raw=%02x -> mask=%02x\n",
                    cia->id == CIA_PORT_A ? 'A' : 'B',
                    (unsigned)val,
                    (unsigned)cia->icr_mask);
            cia_sync_irq_line(cia);
            return;

        case CIA_REG_CRA:
            cia->cra = (uint8_t)(val & (uint8_t)~CIA_CRA_LOAD);

            if (val & CIA_CRA_LOAD)
                cia->ta_counter = cia->ta_latch;

            if (!(cia->cra & CIA_CRA_START) && (cia->cra & CIA_CRA_RUNMODE))
                cia->ta_counter = cia->ta_latch;

            return;

        case CIA_REG_CRB:
            cia->crb = (uint8_t)(val & (uint8_t)~CIA_CRB_LOAD);

            if (val & CIA_CRB_LOAD)
                cia->tb_counter = cia->tb_latch;

            if (!(cia->crb & CIA_CRB_START) && (cia->crb & CIA_CRB_RUNMODE))
                cia->tb_counter = cia->tb_latch;

            return;

        default:
            return;
    }
}