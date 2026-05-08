#include "chipset/cia/cia_serial.h"

#include "chipset/cia/cia.h"
#include "chipset/cia/cia_interrupt.h"

static void cia_serial_start_output(CIA *cia)
{
    if (cia->serial_out_busy || !cia->serial_out_buffer_full)
        return;

    cia->serial_out_shift = cia->serial_out_buffer;
    cia->serial_out_bits = 8u;
    cia->serial_out_busy = 1u;
    cia->serial_out_buffer_full = 0u;
}

void cia_serial_reset(CIA *cia)
{
    cia->sdr = 0x00u;
    cia->sdr_full = 0u;
    cia->sp_input_level = 1u;
    cia->sp_output_level = 1u;
    cia->cnt_output_level = 1u;
    cia->serial_in_shift = 0x00u;
    cia->serial_in_bits = 0u;
    cia->serial_out_shift = 0x00u;
    cia->serial_out_bits = 0u;
    cia->serial_out_busy = 0u;
    cia->serial_out_buffer = 0x00u;
    cia->serial_out_buffer_full = 0u;
}

void cia_serial_set_sp_input(CIA *cia, uint8_t level)
{
    cia->sp_input_level = level ? 1u : 0u;
}

void cia_serial_on_cnt_rising(CIA *cia)
{
    if (cia->serial_out_busy || cia->serial_out_buffer_full)
        return;

    cia->serial_in_shift = (uint8_t)((cia->serial_in_shift << 1) |
                                     (cia->sp_input_level ? 1u : 0u));

    if (++cia->serial_in_bits < 8u)
        return;

    cia->sdr = cia->serial_in_shift;
    cia->sdr_full = 1u;
    cia->serial_in_shift = 0x00u;
    cia->serial_in_bits = 0u;
    cia_interrupt_raise(cia, CIA_ICR_SP);
}

void cia_serial_on_timer_a_underflow(CIA *cia)
{
    uint8_t bit;

    if (!(cia->cra & CIA_CRA_SPMODE))
        return;

    cia_serial_start_output(cia);
    if (!cia->serial_out_busy)
        return;

    bit = (uint8_t)((cia->serial_out_shift >> 7) & 1u);
    cia->sp_output_level = bit;

    cia->cnt_output_level = 0u;
    cia->cnt_output_level = 1u;

    cia->serial_out_shift <<= 1;
    cia->serial_out_bits--;

    if (cia->serial_out_bits != 0u)
        return;

    cia->serial_out_busy = 0u;
    cia->sp_output_level = 1u;
    cia_interrupt_raise(cia, CIA_ICR_SP);
    cia_serial_start_output(cia);
}

void cia_serial_write_sdr(CIA *cia, uint8_t value)
{
    cia->sdr = value;

    if (!(cia->cra & CIA_CRA_SPMODE))
        return;

    cia->serial_out_buffer = value;
    cia->serial_out_buffer_full = 1u;
    cia_serial_start_output(cia);
}

int cia_serial_receive_byte(CIA *cia, uint8_t value)
{
    if (cia->sdr_full)
        return 0;

    cia->sdr = value;
    cia->sdr_full = 1u;
    cia_interrupt_raise(cia, CIA_ICR_SP);
    return 1;
}

uint8_t cia_serial_sp_output_level(const CIA *cia)
{
    return cia->sp_output_level ? 1u : 0u;
}

uint8_t cia_serial_cnt_output_level(const CIA *cia)
{
    return cia->cnt_output_level ? 1u : 0u;
}
