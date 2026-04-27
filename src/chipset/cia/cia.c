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

    if (cia_irq_pending(cia))
    {
        paula_irq_raise(cia->paula, cia->paula_irq_bit);
        if (!cia->irq_asserted)
        {
            kprintf("[CIA%c-IRQ] raised data=%02x mask=%02x\n",
                    cia->id == CIA_PORT_A ? 'A' : 'B',
                    (unsigned)cia->icr_data,
                    (unsigned)cia->icr_mask);
        }
        cia->irq_asserted = 1u;
    }
    else
    {
        paula_irq_clear(cia->paula, cia->paula_irq_bit);
        if (cia->irq_asserted)
        {
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
    cia->pra = 0xFFu;
    cia->prb = 0xFFu;
    cia->ddra = 0x00u;
    cia->ddrb = 0x00u;

    cia->ext_pra = 0xFFu;
    cia->ext_prb = 0xFFu;

    cia->sdr = 0x00u;

    cia->icr_mask = 0x00u;
    cia->icr_data = 0x00u;

    cia->cra = 0x00u;
    cia->crb = 0x00u;

    cia->ta_latch = 0xFFFFu;
    cia->ta_counter = 0xFFFFu;
    cia->tb_latch = 0xFFFFu;
    cia->tb_counter = 0xFFFFu;

    cia->irq_asserted = 0u;
}

static int cia_timer_advance(uint16_t *counter,
                             uint16_t latch,
                             bool continuous,
                             uint64_t ticks)
{
    uint32_t c;

    if (ticks == 0)
        return 0;

    c = (uint32_t)(*counter);

    if (ticks <= c)
    {
        *counter = (uint16_t)(c - (uint32_t)ticks);
        return 0;
    }

    ticks -= (uint64_t)c + 1u;

    if (!continuous)
    {
        *counter = 0;
        return 1;
    }

    *counter = latch;

    if (ticks > 0)
    {
        if (ticks <= (uint32_t)(*counter))
            *counter = (uint16_t)((uint32_t)(*counter) - (uint32_t)ticks);
        else
            *counter = 0;
    }

    return 1;
}

static int cia_timer_advance_events(uint16_t *counter,
                                    uint16_t latch,
                                    bool continuous,
                                    uint32_t events)
{
    int underflows = 0;

    while (events > 0)
    {
        uint32_t until_underflow = (uint32_t)(*counter) + 1u;

        if (events < until_underflow)
        {
            *counter = (uint16_t)(*counter - (uint16_t)events);
            break;
        }

        events -= until_underflow;
        underflows++;

        if (!continuous)
        {
            *counter = 0;
            break;
        }

        *counter = latch;
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
    cia->irq_level = (id == CIA_PORT_A) ? 2u : 6u;
    cia->paula_irq_bit = (id == CIA_PORT_A) ? (uint16_t)PAULA_INT_PORTS
                                            : (uint16_t)PAULA_INT_EXTER;
    cia->paula = NULL;

    cia_reset(cia);
}

void cia_reset(CIA *cia)
{
    CIA_ID saved_id = cia->id;
    uint8_t saved_irq = cia->irq_level;
    uint16_t saved_paulabit = cia->paula_irq_bit;
    struct Paula *saved_paula = cia->paula;

    cia_reset_core_state(cia);

    cia->id = saved_id;
    cia->irq_level = saved_irq;
    cia->paula_irq_bit = saved_paulabit;
    cia->paula = saved_paula;

    cia_tod_reset(&cia->tod,
                  (cia->id == CIA_PORT_A) ? CIA_A_TOD_TICKS_PER_INCREMENT : CIA_B_TOD_TICKS_PER_INCREMENT);

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
    /*
     * CIA-A PRA:
     * bits 0-1 are normal output/input lines controlled by PRA/DDRA.
     * bits 2-7 are mostly external/status lines on Amiga and must not be
     * overridden by guest writes to PRA/DDRA.
     *
     * Important floppy bits:
     * bit 2 = /DSKCHG
     * bit 3 = /WPRO
     * bit 4 = /TK0
     * bit 5 = /RDY
     */
    if (cia->id == CIA_PORT_A)
    {
        uint8_t low = (uint8_t)((cia->pra & cia->ddra & 0x03u) |
                                (cia->ext_pra & (uint8_t)~cia->ddra & 0x03u));

        return (uint8_t)(low | (cia->ext_pra & 0xFCu));
    }

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
    if ((cia->cra & CIA_CRA_START) && !(cia->cra & CIA_CRA_INMODE))
    {
        bool continuous = (cia->cra & CIA_CRA_RUNMODE) ? false : true;

        uf_a = cia_timer_advance(&cia->ta_counter,
                                 cia->ta_latch,
                                 continuous,
                                 ticks);
        if (uf_a > 0)
        {
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
    if (cia->crb & CIA_CRB_START)
    {
        uint8_t inmode = (uint8_t)((cia->crb >> 5) & 0x03u);
        bool continuous = (cia->crb & CIA_CRB_RUNMODE) ? false : true;

        if (inmode == 0u)
        {
            uf_b = cia_timer_advance(&cia->tb_counter,
                                     cia->tb_latch,
                                     continuous,
                                     ticks);

            kprintf("[CIA%c-TB-ADV] ret=%d ticks=%llu counter=%04x latch=%04x\n",
                    cia->id == CIA_PORT_A ? 'A' : 'B',
                    uf_b,
                    (unsigned long long)ticks,
                    (unsigned)cia->tb_counter,
                    (unsigned)cia->tb_latch);
                }
        else if (inmode == 2u && uf_a > 0)
        {
            uf_b = cia_timer_advance_events(&cia->tb_counter,
                                            cia->tb_latch,
                                            continuous,
                                            (uint32_t)uf_a);
        }

        if (uf_b > 0)
        {
            kprintf("[CIA%c-TB-FIRE] uf_b=%d ticks=%llu counter=%04x latch=%04x crb=%02x icr_data=%02x mask=%02x\n",
                    cia->id == CIA_PORT_A ? 'A' : 'B',
                    uf_b,
                    (unsigned long long)ticks,
                    (unsigned)cia->tb_counter,
                    (unsigned)cia->tb_latch,
                    (unsigned)cia->crb,
                    (unsigned)cia->icr_data,
                    (unsigned)cia->icr_mask);

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
    switch (reg & 0x0Fu)
    {

    case CIA_REG_PRA:
    {
        uint8_t v = cia_port_a_value(cia);
        /* log only on value change to avoid flooding tight polling loops */
        static uint8_t last_pra_a, last_pra_b;
        uint8_t *last = (cia->id == CIA_PORT_A) ? &last_pra_a : &last_pra_b;
        if (v != *last)
        {
            kprintf("[CIA%c-PRA-R] -> %02x  (pra=%02x ddra=%02x ext=%02x)\n",
                    cia->id == CIA_PORT_A ? 'A' : 'B',
                    (unsigned)v, (unsigned)cia->pra,
                    (unsigned)cia->ddra, (unsigned)cia->ext_pra);
            *last = v;
        }
        return v;
    }

    case CIA_REG_PRB:
    {
        uint8_t v = cia_port_b_value(cia);
        static uint8_t last_prb_a, last_prb_b;
        uint8_t *last = (cia->id == CIA_PORT_A) ? &last_prb_a : &last_prb_b;
        if (v != *last)
        {
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

    case CIA_REG_ICR:
    {
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
    switch (reg & 0x0Fu)
    {

    case CIA_REG_PRA:
    {
        uint8_t old = cia->pra;
        uint8_t stored = val;

        if (cia->id == CIA_PORT_A)
            stored = (uint8_t)((cia->pra & 0xFCu) | (val & 0x03u));

        cia->pra = stored;

        kprintf("[CIA%c-PRA-W] old=%02x new=%02x stored=%02x ddra=%02x ext=%02x result=%02x\n",
                cia->id == CIA_PORT_A ? 'A' : 'B',
                (unsigned)old,
                (unsigned)val,
                (unsigned)stored,
                (unsigned)cia->ddra,
                (unsigned)cia->ext_pra,
                (unsigned)cia_port_a_value(cia));
        return;
    }

    case CIA_REG_PRB:
    {
        uint8_t old = cia->prb;
        cia->prb = val;

        kprintf("[CIA%c-PRB-W] old=%02x new=%02x ddrb=%02x ext=%02x result=%02x\n",
                cia->id == CIA_PORT_A ? 'A' : 'B',
                (unsigned)old,
                (unsigned)val,
                (unsigned)cia->ddrb,
                (unsigned)cia->ext_prb,
                (unsigned)cia_port_b_value(cia));
        return;
    }

    case CIA_REG_DDRA:
    {
        uint8_t stored = val;

        /*
         * CIA-A: keep bits 2-7 effectively external.
         * Only bits 0-1 should participate normally in PRA output selection.
         */
        if (cia->id == CIA_PORT_A)
        {
            stored = (uint8_t)(val & 0x03u);
        }

        kprintf("[CIA%c-DDRA-W] old=%02x new=%02x stored=%02x\n",
                cia->id == CIA_PORT_A ? 'A' : 'B',
                (unsigned)cia->ddra,
                (unsigned)val,
                (unsigned)stored);

        cia->ddra = stored;
        return;
    }

    case CIA_REG_DDRB:
        kprintf("[CIA%c-DDRB-W] old=%02x new=%02x\n",
                cia->id == CIA_PORT_A ? 'A' : 'B',
                (unsigned)cia->ddrb, (unsigned)val);
        cia->ddrb = val;
        return;

    case CIA_REG_TALO:
        cia->ta_latch = (uint16_t)((cia->ta_latch & 0xFF00u) | (uint16_t)val);
        kprintf("[CIA%c-TALO-W] val=%02x latch=%04x cra=%02x\n",
                cia->id == CIA_PORT_A ? 'A' : 'B',
                val, cia->ta_latch, cia->cra);
        return;

    case CIA_REG_TAHI:
        cia->ta_latch = (uint16_t)((cia->ta_latch & 0x00FFu) | ((uint16_t)val << 8));
        if (!(cia->cra & CIA_CRA_START)) {
            cia->ta_counter = cia->ta_latch;
            /* CIA-8520: writing TAHI while stopped auto-starts in one-shot mode */
            if (cia->cra & CIA_CRA_RUNMODE)
                cia->cra |= CIA_CRA_START;
        }
        kprintf("[CIA%c-TAHI-W] val=%02x latch=%04x counter=%04x cra=%02x\n",
                cia->id == CIA_PORT_A ? 'A' : 'B',
                val, cia->ta_latch, cia->ta_counter, cia->cra);
        return;

    case CIA_REG_TBLO:
        cia->tb_latch = (uint16_t)((cia->tb_latch & 0xFF00u) | (uint16_t)val);
        kprintf("[CIA%c-TBLO-W] val=%02x latch=%04x counter=%04x crb=%02x\n",
                cia->id == CIA_PORT_A ? 'A' : 'B',
                val, cia->tb_latch, cia->tb_counter, cia->crb);
        return;

    case CIA_REG_TBHI:
        cia->tb_latch = (uint16_t)((cia->tb_latch & 0x00FFu) | ((uint16_t)val << 8));
        if (!(cia->crb & CIA_CRB_START)) {
            cia->tb_counter = cia->tb_latch;
            /* CIA-8520: writing TBHI while stopped auto-starts in one-shot mode */
            if (cia->crb & CIA_CRB_RUNMODE)
                cia->crb |= CIA_CRB_START;
        }
        kprintf("[CIA%c-TBHI-W] val=%02x latch=%04x counter=%04x crb=%02x\n",
                cia->id == CIA_PORT_A ? 'A' : 'B',
                val, cia->tb_latch, cia->tb_counter, cia->crb);

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
    {
        uint8_t old = cia->cra;

        cia->cra = (uint8_t)(val & (uint8_t)~CIA_CRA_LOAD);

        if (val & CIA_CRA_LOAD)
            cia->ta_counter = cia->ta_latch;

        if (!(cia->cra & CIA_CRA_START) && (cia->cra & CIA_CRA_RUNMODE))
            cia->ta_counter = cia->ta_latch;

        kprintf("[CIA%c-CRA-W] old=%02x new=%02x stored=%02x ta=%04x latch=%04x\n",
                cia->id == CIA_PORT_A ? 'A' : 'B',
                (unsigned)old,
                (unsigned)val,
                (unsigned)cia->cra,
                (unsigned)cia->ta_counter,
                (unsigned)cia->ta_latch);
        return;
    }

    case CIA_REG_CRB:
    {
        uint8_t old = cia->crb;

        cia->crb = (uint8_t)(val & (uint8_t)~CIA_CRB_LOAD);

        if (val & CIA_CRB_LOAD)
            cia->tb_counter = cia->tb_latch;

        if (!(cia->crb & CIA_CRB_START) && (cia->crb & CIA_CRB_RUNMODE))
            cia->tb_counter = cia->tb_latch;

        kprintf("[CIA%c-CRB-W] old=%02x new=%02x stored=%02x tb=%04x latch=%04x\n",
                cia->id == CIA_PORT_A ? 'A' : 'B',
                (unsigned)old,
                (unsigned)val,
                (unsigned)cia->crb,
                (unsigned)cia->tb_counter,
                (unsigned)cia->tb_latch);
        return;
    }

    default:
        return;
    }
}