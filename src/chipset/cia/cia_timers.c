#include "chipset/cia/cia_timers.h"

#include "chipset/cia/cia.h"
#include "chipset/cia/cia_interrupt.h"
#include "chipset/cia/cia_serial.h"
#include "support.h"

#include <stdbool.h>

static int cia_timer_advance(uint16_t *counter,
                             uint16_t latch,
                             bool continuous,
                             uint64_t ticks)
{
    uint32_t c;
    uint32_t cycle;
    uint32_t rem;
    uint64_t underflows = 1u;

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

    cycle = (uint32_t)latch + 1u;
    underflows += ticks / (uint64_t)cycle;
    rem = (uint32_t)(ticks % (uint64_t)cycle);
    *counter = (rem == 0u) ? latch : (uint16_t)(latch - rem);

    return (int)underflows;
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

void cia_timers_reset(CIA *cia)
{
    cia->cra = 0x00u;
    cia->crb = 0x00u;
    cia->ta_latch = 0xFFFFu;
    cia->ta_counter = 0xFFFFu;
    cia->tb_latch = 0xFFFFu;
    cia->tb_counter = 0xFFFFu;
    cia->pb_timer_out = 0xC0u;
    cia->pb_pulse_mask = 0x00u;
}

static void cia_timer_apply_underflow_output(CIA *cia, uint8_t control, int underflows)
{
    uint8_t mask = (control == cia->cra) ? 0x40u : 0x80u;

    if (!(control & CIA_CRA_PBON))
        return;

    if (control & CIA_CRA_OUTMODE)
    {
        if (underflows & 1)
            cia->pb_timer_out ^= mask;
        cia->pb_pulse_mask &= (uint8_t)~mask;
        return;
    }

    cia->pb_timer_out &= (uint8_t)~mask;
    cia->pb_pulse_mask |= mask;
}

void cia_timers_advance_pb_outputs(CIA *cia)
{
    if ((cia->pb_pulse_mask & 0x40u) != 0u)
        cia->pb_timer_out |= 0x40u;
    if ((cia->pb_pulse_mask & 0x80u) != 0u)
        cia->pb_timer_out |= 0x80u;

    cia->pb_pulse_mask = 0u;
}

void cia_timers_step(CIA *cia, uint64_t ticks)
{
    int uf_a = 0;
    int uf_b = 0;

    if (ticks == 0)
        return;

    if ((cia->cra & CIA_CRA_START) && !(cia->cra & CIA_CRA_INMODE))
    {
        bool continuous = (cia->cra & CIA_CRA_RUNMODE) ? false : true;

        uf_a = cia_timer_advance(&cia->ta_counter,
                                 cia->ta_latch,
                                 continuous,
                                 ticks);
        if (uf_a > 0)
        {
            cia_interrupt_raise(cia, CIA_ICR_TA);
            cia_timer_apply_underflow_output(cia, cia->cra, uf_a);
            for (int i = 0; i < uf_a; i++)
                cia_serial_on_timer_a_underflow(cia);

            if (cia->cra & CIA_CRA_RUNMODE)
                cia->cra &= (uint8_t)~CIA_CRA_START;
        }
    }

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
        }
        else if ((inmode == 2u || inmode == 3u) &&
                 uf_a > 0 &&
                 (inmode != 3u || cia->cnt_level))
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

            cia_interrupt_raise(cia, CIA_ICR_TB);
            cia_timer_apply_underflow_output(cia, cia->crb, uf_b);

            if (cia->crb & CIA_CRB_RUNMODE)
                cia->crb &= (uint8_t)~CIA_CRB_START;
        }
    }
}

void cia_timers_count_cnt(CIA *cia, uint32_t pulses)
{
    int uf_a = 0;
    int uf_b = 0;

    if (pulses == 0)
        return;

    if ((cia->cra & CIA_CRA_START) && (cia->cra & CIA_CRA_INMODE))
    {
        bool continuous = (cia->cra & CIA_CRA_RUNMODE) ? false : true;

        uf_a = cia_timer_advance_events(&cia->ta_counter,
                                        cia->ta_latch,
                                        continuous,
                                        pulses);
        if (uf_a > 0)
        {
            cia_interrupt_raise(cia, CIA_ICR_TA);
            cia_timer_apply_underflow_output(cia, cia->cra, uf_a);
            for (int i = 0; i < uf_a; i++)
                cia_serial_on_timer_a_underflow(cia);

            if (cia->cra & CIA_CRA_RUNMODE)
                cia->cra &= (uint8_t)~CIA_CRA_START;
        }
    }

    if (cia->crb & CIA_CRB_START)
    {
        uint8_t inmode = (uint8_t)((cia->crb >> 5) & 0x03u);
        bool continuous = (cia->crb & CIA_CRB_RUNMODE) ? false : true;

        if (inmode == 1u)
        {
            uf_b = cia_timer_advance_events(&cia->tb_counter,
                                            cia->tb_latch,
                                            continuous,
                                            pulses);
        }
        else if (inmode == 3u && uf_a > 0)
        {
            uf_b = cia_timer_advance_events(&cia->tb_counter,
                                            cia->tb_latch,
                                            continuous,
                                            (uint32_t)uf_a);
        }

        if (uf_b > 0)
        {
            cia_interrupt_raise(cia, CIA_ICR_TB);
            cia_timer_apply_underflow_output(cia, cia->crb, uf_b);

            if (cia->crb & CIA_CRB_RUNMODE)
                cia->crb &= (uint8_t)~CIA_CRB_START;
        }
    }
}

int cia_timers_read_reg(CIA *cia, uint8_t reg, uint8_t *value)
{
    switch (reg & 0x0Fu)
    {
    case CIA_REG_TALO:
        *value = (uint8_t)(cia->ta_counter & 0x00FFu);
        return 1;

    case CIA_REG_TAHI:
        *value = (uint8_t)((cia->ta_counter >> 8) & 0x00FFu);
        return 1;

    case CIA_REG_TBLO:
        *value = (uint8_t)(cia->tb_counter & 0x00FFu);
        return 1;

    case CIA_REG_TBHI:
        *value = (uint8_t)((cia->tb_counter >> 8) & 0x00FFu);
        return 1;

    case CIA_REG_CRA:
        *value = cia->cra;
        return 1;

    case CIA_REG_CRB:
        *value = cia->crb;
        return 1;

    default:
        return 0;
    }
}

int cia_timers_write_reg(CIA *cia, uint8_t reg, uint8_t val)
{
    switch (reg & 0x0Fu)
    {
    case CIA_REG_TALO:
        cia->ta_latch = (uint16_t)((cia->ta_latch & 0xFF00u) | (uint16_t)val);
        kprintf("[CIA%c-TALO-W] val=%02x latch=%04x cra=%02x\n",
                cia->id == CIA_PORT_A ? 'A' : 'B',
                val, cia->ta_latch, cia->cra);
        return 1;

    case CIA_REG_TAHI:
        cia->ta_latch = (uint16_t)((cia->ta_latch & 0x00FFu) | ((uint16_t)val << 8));
        if (!(cia->cra & CIA_CRA_START)) {
            cia->ta_counter = cia->ta_latch;
            if (cia->cra & CIA_CRA_RUNMODE)
                cia->cra |= CIA_CRA_START;
        }
        kprintf("[CIA%c-TAHI-W] val=%02x latch=%04x counter=%04x cra=%02x\n",
                cia->id == CIA_PORT_A ? 'A' : 'B',
                val, cia->ta_latch, cia->ta_counter, cia->cra);
        return 1;

    case CIA_REG_TBLO:
        cia->tb_latch = (uint16_t)((cia->tb_latch & 0xFF00u) | (uint16_t)val);
        kprintf("[CIA%c-TBLO-W] val=%02x latch=%04x counter=%04x crb=%02x\n",
                cia->id == CIA_PORT_A ? 'A' : 'B',
                val, cia->tb_latch, cia->tb_counter, cia->crb);
        return 1;

    case CIA_REG_TBHI:
        cia->tb_latch = (uint16_t)((cia->tb_latch & 0x00FFu) | ((uint16_t)val << 8));
        if (!(cia->crb & CIA_CRB_START)) {
            cia->tb_counter = cia->tb_latch;
            if (cia->crb & CIA_CRB_RUNMODE)
                cia->crb |= CIA_CRB_START;
        }
        kprintf("[CIA%c-TBHI-W] val=%02x latch=%04x counter=%04x crb=%02x\n",
                cia->id == CIA_PORT_A ? 'A' : 'B',
                val, cia->tb_latch, cia->tb_counter, cia->crb);
        return 1;

    case CIA_REG_CRA:
    {
        uint8_t old = cia->cra;

        cia->cra = (uint8_t)(val & (uint8_t)~CIA_CRA_LOAD);
        if (cia->cra & CIA_CRA_PBON)
            cia->pb_timer_out |= 0x40u;

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
        return 1;
    }

    case CIA_REG_CRB:
    {
        uint8_t old = cia->crb;

        cia->crb = (uint8_t)(val & (uint8_t)~CIA_CRB_LOAD);
        if (cia->crb & CIA_CRB_PBON)
            cia->pb_timer_out |= 0x80u;

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
        return 1;
    }

    default:
        return 0;
    }
}
