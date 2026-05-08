#include "chipset/cia/cia.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK_EQ(msg, exp, got)                                                     \
    do {                                                                            \
        unsigned _exp = (unsigned)(exp);                                            \
        unsigned _got = (unsigned)(got);                                            \
        if (_exp != _got) {                                                         \
            fprintf(stderr, "%s: expected=%u got=%u\n", msg, _exp, _got);           \
            exit(1);                                                                \
        }                                                                           \
    } while (0)

typedef struct Paula Paula;

void paula_irq_raise(Paula *p, uint16_t bits)
{
    (void)p;
    (void)bits;
}

void paula_irq_clear(Paula *p, uint16_t bits)
{
    (void)p;
    (void)bits;
}

static void test_timer_a_cnt_mode(void)
{
    CIA cia;

    cia_init(&cia, CIA_PORT_A);
    cia_write_reg(&cia, CIA_REG_TALO, 0x02u);
    cia_write_reg(&cia, CIA_REG_TAHI, 0x00u);
    cia_write_reg(&cia, CIA_REG_ICR, (uint8_t)(CIA_ICR_SETCLR | CIA_ICR_TA));
    cia_write_reg(&cia, CIA_REG_CRA, (uint8_t)(CIA_CRA_START | CIA_CRA_INMODE));

    cia_set_cnt_level(&cia, 0u);
    cia_set_cnt_level(&cia, 1u);
    CHECK_EQ("timer A cnt pulse 1", 1u, cia.ta_counter);

    cia_set_cnt_level(&cia, 0u);
    cia_set_cnt_level(&cia, 1u);
    CHECK_EQ("timer A cnt pulse 2", 0u, cia.ta_counter);

    cia_set_cnt_level(&cia, 0u);
    cia_set_cnt_level(&cia, 1u);
    CHECK_EQ("timer A underflow irq", 1u, cia_irq_pending(&cia));
}

static void test_timer_b_cnt_mode(void)
{
    CIA cia;

    cia_init(&cia, CIA_PORT_B);
    cia_write_reg(&cia, CIA_REG_TBLO, 0x01u);
    cia_write_reg(&cia, CIA_REG_TBHI, 0x00u);
    cia_write_reg(&cia, CIA_REG_ICR, (uint8_t)(CIA_ICR_SETCLR | CIA_ICR_TB));
    cia_write_reg(&cia, CIA_REG_CRB, (uint8_t)(CIA_CRB_START | CIA_CRB_INMODE0));

    cia_set_cnt_level(&cia, 0u);
    cia_set_cnt_level(&cia, 1u);
    CHECK_EQ("timer B cnt pulse 1", 0u, cia.tb_counter);

    cia_set_cnt_level(&cia, 0u);
    cia_set_cnt_level(&cia, 1u);
    CHECK_EQ("timer B underflow irq", 1u, cia_irq_pending(&cia));
}

static void test_timer_b_ta_underflow_gated_by_cnt(void)
{
    CIA cia;

    cia_init(&cia, CIA_PORT_B);
    cia_write_reg(&cia, CIA_REG_TALO, 0x00u);
    cia_write_reg(&cia, CIA_REG_TAHI, 0x00u);
    cia_write_reg(&cia, CIA_REG_TBLO, 0x01u);
    cia_write_reg(&cia, CIA_REG_TBHI, 0x00u);
    cia_write_reg(&cia, CIA_REG_ICR, (uint8_t)(CIA_ICR_SETCLR | CIA_ICR_TB));
    cia_write_reg(&cia, CIA_REG_CRA, CIA_CRA_START);
    cia_write_reg(&cia, CIA_REG_CRB, (uint8_t)(CIA_CRB_START | CIA_CRB_INMODE0 | CIA_CRB_INMODE1));

    cia_set_cnt_level(&cia, 0u);
    cia_step(&cia, 1u);
    CHECK_EQ("timer B gated by low CNT", 1u, cia.tb_counter);

    cia_set_cnt_level(&cia, 1u);
    cia_step(&cia, 1u);
    CHECK_EQ("timer B counts TA underflow when CNT high", 0u, cia.tb_counter);

    cia_step(&cia, 1u);
    CHECK_EQ("timer B gated underflow irq", 1u, cia_irq_pending(&cia));
}

static void test_flag_negative_edge_irq(void)
{
    CIA cia;

    cia_init(&cia, CIA_PORT_B);
    cia_write_reg(&cia, CIA_REG_ICR, (uint8_t)(CIA_ICR_SETCLR | CIA_ICR_FLG));

    cia_set_flag_level(&cia, 1u);
    cia_set_flag_level(&cia, 0u);
    CHECK_EQ("flag irq on falling edge", 1u, cia_irq_pending(&cia));

    (void)cia_read_reg(&cia, CIA_REG_ICR);
    CHECK_EQ("flag irq clear on ICR read", 0u, cia_irq_pending(&cia));
}

static void test_tod_write_stops_until_low_byte(void)
{
    CIA cia;

    cia_init(&cia, CIA_PORT_A);
    cia_step(&cia, CIA_A_TOD_TICKS_PER_INCREMENT);
    CHECK_EQ("tod increments before write", 1u, cia.tod.counter);

    cia_write_reg(&cia, CIA_REG_TODHI, 0x12u);
    cia_step(&cia, CIA_A_TOD_TICKS_PER_INCREMENT);
    CHECK_EQ("tod stopped after high-byte write", 0x120001u, cia.tod.counter);

    cia_write_reg(&cia, CIA_REG_TODMID, 0x34u);
    cia_step(&cia, CIA_A_TOD_TICKS_PER_INCREMENT);
    CHECK_EQ("tod still stopped before low-byte write", 0x123401u, cia.tod.counter);

    cia_write_reg(&cia, CIA_REG_TODLO, 0x56u);
    CHECK_EQ("tod programmed final value", 0x123456u, cia.tod.counter);

    cia_step(&cia, CIA_A_TOD_TICKS_PER_INCREMENT);
    CHECK_EQ("tod resumes after low-byte write", 0x123457u, cia.tod.counter);
}

static void test_pbon_toggle_and_pulse(void)
{
    CIA cia;

    cia_init(&cia, CIA_PORT_B);
    cia_write_reg(&cia, CIA_REG_TALO, 0x00u);
    cia_write_reg(&cia, CIA_REG_TAHI, 0x00u);
    cia_write_reg(&cia, CIA_REG_CRA, (uint8_t)(CIA_CRA_START | CIA_CRA_PBON | CIA_CRA_OUTMODE));

    CHECK_EQ("pb6 starts high", 0x40u, cia_port_b_value(&cia) & 0x40u);
    cia_step(&cia, 1u);
    CHECK_EQ("pb6 toggles low", 0x00u, cia_port_b_value(&cia) & 0x40u);
    cia_step(&cia, 1u);
    CHECK_EQ("pb6 toggles high", 0x40u, cia_port_b_value(&cia) & 0x40u);

    cia_init(&cia, CIA_PORT_B);
    cia_write_reg(&cia, CIA_REG_TBLO, 0x00u);
    cia_write_reg(&cia, CIA_REG_TBHI, 0x00u);
    cia_write_reg(&cia, CIA_REG_CRB, (uint8_t)(CIA_CRB_START | CIA_CRB_PBON | CIA_CRB_RUNMODE));

    CHECK_EQ("pb7 starts high", 0x80u, cia_port_b_value(&cia) & 0x80u);
    cia_step(&cia, 1u);
    CHECK_EQ("pb7 pulse low", 0x00u, cia_port_b_value(&cia) & 0x80u);
    cia_step(&cia, 1u);
    CHECK_EQ("pb7 returns high", 0x80u, cia_port_b_value(&cia) & 0x80u);
}

static void test_serial_input_shift(void)
{
    CIA cia;
    uint8_t wire = 0xA5u;

    cia_init(&cia, CIA_PORT_A);
    cia_write_reg(&cia, CIA_REG_ICR, (uint8_t)(CIA_ICR_SETCLR | CIA_ICR_SP));

    for (int i = 0; i < 8; i++)
    {
        cia_set_sp_level(&cia, (uint8_t)((wire >> 7) & 1u));
        cia_set_cnt_level(&cia, 0u);
        cia_set_cnt_level(&cia, 1u);
        wire <<= 1;
    }

    CHECK_EQ("serial input fills SDR", 0xA5u, cia_read_reg(&cia, CIA_REG_SDR));
    CHECK_EQ("serial input irq", 1u, cia_irq_pending(&cia));
}

static void test_serial_output_shift(void)
{
    CIA cia;

    cia_init(&cia, CIA_PORT_A);
    cia_write_reg(&cia, CIA_REG_TALO, 0x00u);
    cia_write_reg(&cia, CIA_REG_TAHI, 0x00u);
    cia_write_reg(&cia, CIA_REG_ICR, (uint8_t)(CIA_ICR_SETCLR | CIA_ICR_SP));
    cia_write_reg(&cia, CIA_REG_CRA, (uint8_t)(CIA_CRA_START | CIA_CRA_SPMODE));
    cia_write_reg(&cia, CIA_REG_SDR, 0x80u);

    CHECK_EQ("serial output idle high", 1u, cia_serial_sp_output_level(&cia));
    cia_step(&cia, 1u);
    CHECK_EQ("serial output first bit high", 1u, cia_serial_sp_output_level(&cia));
    cia_step(&cia, 1u);
    CHECK_EQ("serial output second bit low", 0u, cia_serial_sp_output_level(&cia));

    for (int i = 0; i < 6; i++)
        cia_step(&cia, 1u);

    CHECK_EQ("serial output irq after byte", 1u, cia_irq_pending(&cia));
    CHECK_EQ("serial output idle high", 1u, cia_serial_sp_output_level(&cia));
}

static void test_serial_input_after_output_mode(void)
{
    CIA cia;
    uint8_t wire = 0x3Cu;

    cia_init(&cia, CIA_PORT_A);
    cia_write_reg(&cia, CIA_REG_ICR, (uint8_t)(CIA_ICR_SETCLR | CIA_ICR_SP));
    cia_write_reg(&cia, CIA_REG_TALO, 0x00u);
    cia_write_reg(&cia, CIA_REG_TAHI, 0x00u);
    cia_write_reg(&cia, CIA_REG_CRA, (uint8_t)(CIA_CRA_START | CIA_CRA_SPMODE));
    cia_write_reg(&cia, CIA_REG_SDR, 0x80u);

    for (int i = 0; i < 8; i++)
        cia_step(&cia, 1u);

    CHECK_EQ("serial output complete", 0u, cia.serial_out_busy);
    CHECK_EQ("spmode remains set", CIA_CRA_START | CIA_CRA_SPMODE, cia.cra);

    for (int i = 0; i < 8; i++)
    {
        cia_set_sp_level(&cia, (uint8_t)((wire >> 7) & 1u));
        cia_set_cnt_level(&cia, 0u);
        cia_set_cnt_level(&cia, 1u);
        wire <<= 1;
    }

    CHECK_EQ("serial input still works after output mode", 0x3Cu, cia_read_reg(&cia, CIA_REG_SDR));
}

int main(void)
{
    test_timer_a_cnt_mode();
    test_timer_b_cnt_mode();
    test_timer_b_ta_underflow_gated_by_cnt();
    test_flag_negative_edge_irq();
    test_tod_write_stops_until_low_byte();
    test_pbon_toggle_and_pulse();
    test_serial_input_shift();
    test_serial_output_shift();
    test_serial_input_after_output_mode();

    puts("bellatrix_unit_cia: ok");
    return 0;
}
