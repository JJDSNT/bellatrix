#include "chipset/cia/cia_ports.h"

#include "chipset/cia/cia.h"
#include "chipset/cia/cia_interrupt.h"
#include "chipset/cia/cia_serial.h"
#include "chipset/cia/cia_timers.h"
#include "support.h"

void cia_ports_reset(CIA *cia)
{
    cia->pra = 0xFFu;
    cia->prb = 0xFFu;
    cia->ddra = 0x00u;
    cia->ddrb = 0x00u;
    cia->ext_pra = 0xFFu;
    cia->ext_prb = 0xFFu;
}

void cia_ports_apply_defaults(CIA *cia)
{
    if (!cia)
        return;

    switch (cia->id)
    {
    case CIA_PORT_A:
    case CIA_PORT_B:
        cia->ext_pra = 0xFFu;
        cia->ext_prb = 0xFFu;
        return;
    default:
        return;
    }
}

void cia_set_external_pra(CIA *cia, uint8_t value)
{
    cia->ext_pra = value;
}

void cia_set_external_prb(CIA *cia, uint8_t value)
{
    cia->ext_prb = value;
}

void cia_set_sp_level(CIA *cia, uint8_t level)
{
    cia_serial_set_sp_input(cia, level);
}

void cia_set_cnt_level(CIA *cia, uint8_t level)
{
    uint8_t new_level = level ? 1u : 0u;
    uint8_t old_level = cia->cnt_level;

    cia->cnt_level = new_level;

    if (!old_level && new_level) {
        cia_serial_on_cnt_rising(cia);
        cia_timers_count_cnt(cia, 1u);
    }
}

void cia_set_flag_level(CIA *cia, uint8_t level)
{
    uint8_t new_level = level ? 1u : 0u;
    uint8_t old_level = cia->flag_level;

    cia->flag_level = new_level;

    if (old_level && !new_level)
        cia_interrupt_raise(cia, CIA_ICR_FLG);
}

void cia_pulse_cnt(CIA *cia, uint32_t pulses)
{
    cia_timers_count_cnt(cia, pulses);
}

int cia_ports_read_reg(CIA *cia, uint8_t reg, uint8_t *value)
{
    switch (reg & 0x0Fu)
    {
    case CIA_REG_PRA:
        *value = cia_port_a_value(cia);
        return 1;

    case CIA_REG_PRB:
        *value = cia_port_b_value(cia);
        return 1;

    case CIA_REG_DDRA:
        *value = cia->ddra;
        return 1;

    case CIA_REG_DDRB:
        *value = cia->ddrb;
        return 1;

    default:
        return 0;
    }
}

int cia_ports_write_reg(CIA *cia, uint8_t reg, uint8_t value)
{
    switch (reg & 0x0Fu)
    {
    case CIA_REG_PRA:
        cia->pra = value;
        return 1;

    case CIA_REG_PRB:
        cia->prb = value;
        return 1;

    case CIA_REG_DDRA:
        cia->ddra = value;
        return 1;

    case CIA_REG_DDRB:
        cia->ddrb = value;
        return 1;

    default:
        return 0;
    }
}

uint8_t cia_port_a_value(const CIA *cia)
{
    if (cia->id == CIA_PORT_A)
    {
        uint8_t low = (uint8_t)((cia->pra & cia->ddra & 0x03u) |
                                (cia->ext_pra & (uint8_t)~cia->ddra & 0x03u));

        return (uint8_t)(low | (cia->ext_pra & 0xFCu));
    }

    return (uint8_t)((cia->pra & cia->ddra) |
                     (cia->ext_pra & (uint8_t)~cia->ddra));
}

uint8_t cia_port_b_value(const CIA *cia)
{
    uint8_t value = (uint8_t)((cia->prb & cia->ddrb) |
                              (cia->ext_prb & (uint8_t)~cia->ddrb));

    if (cia->cra & CIA_CRA_PBON)
    {
        if (cia->pb_timer_out & 0x40u)
            value |= 0x40u;
        else
            value &= (uint8_t)~0x40u;
    }

    if (cia->crb & CIA_CRB_PBON)
    {
        if (cia->pb_timer_out & 0x80u)
            value |= 0x80u;
        else
            value &= (uint8_t)~0x80u;
    }

    return value;
}
