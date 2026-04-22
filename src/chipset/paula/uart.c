#include "chipset/paula/uart.h"

#include <string.h>

static uint32_t uart_cycles_per_bit(const UARTState *u)
{
    uint32_t v = (uint32_t)(u->serper & 0x7FFFu) + 1u;
    return v ? v : 1u;
}

static uint32_t uart_frame_cycles(const UARTState *u, uint16_t word)
{
    (void)word;
    return 10u * uart_cycles_per_bit(u);
}

static void uart_raise_irq(UARTState *u, uint16_t mask)
{
    if (u->irq_raise_cb) {
        u->irq_raise_cb(u->opaque, mask);
    }
}

static void uart_emit_tx_byte(UARTState *u, uint16_t word)
{
    if (u->tx_cb) {
        u->tx_cb(u->opaque, (uint8_t)(word & 0xFFu));
    }
}

static void uart_start_tx_shift(UARTState *u)
{
    if (!u->tx_buffer_valid || u->tx_shift_busy) {
        return;
    }

    u->tx_shift_reg = u->tx_buffer;
    u->tx_shift_busy = true;
    u->tx_cycles_remaining = uart_frame_cycles(u, u->tx_shift_reg);
    u->tx_buffer_valid = false;

    if (u->tx_instant) {
        uart_emit_tx_byte(u, u->tx_shift_reg);
    }

    uart_raise_irq(u, UART_INTREQ_TBE);
}

void uart_init(UARTState *u, void *opaque,
               uart_tx_byte_cb tx_cb,
               uart_irq_raise_cb irq_raise_cb)
{
    memset(u, 0, sizeof(*u));
    u->opaque = opaque;
    u->tx_cb = tx_cb;
    u->irq_raise_cb = irq_raise_cb;
    uart_reset(u);
}

void uart_reset(UARTState *u)
{
    u->serper = 0;

    u->tx_buffer = 0;
    u->tx_buffer_valid = false;

    u->tx_shift_reg = 0;
    u->tx_shift_busy = false;
    u->tx_cycles_remaining = 0;

    u->rx_buffer = 0;
    u->rx_buffer_full = false;
    u->overrun = false;
    u->rxd_level = true;

    u->tx_instant = true;
}

void uart_write_serdat(UARTState *u, uint16_t value)
{
    u->tx_buffer = value;
    u->tx_buffer_valid = true;

    if (!u->tx_shift_busy) {
        uart_start_tx_shift(u);
    }
}

void uart_write_serper(UARTState *u, uint16_t value)
{
    u->serper = value;
}

uint16_t uart_read_serdatr(const UARTState *u)
{
    uint16_t v = 0;

    v |= (u->rx_buffer & 0x03FFu);

    if (u->overrun)          v |= 0x8000u;
    if (u->rx_buffer_full)   v |= 0x4000u;
    if (!u->tx_buffer_valid) v |= 0x2000u;
    if (!u->tx_shift_busy)   v |= 0x1000u;
    if (u->rxd_level)        v |= 0x0800u;

    if (!u->rx_buffer_full) {
        v |= 0x0300u;
    }

    return v;
}

void uart_step(UARTState *u, uint32_t cycles)
{
    if (!u->tx_shift_busy) {
        if (u->tx_buffer_valid) {
            uart_start_tx_shift(u);
        }
        return;
    }

    if (cycles >= u->tx_cycles_remaining) {
        u->tx_cycles_remaining = 0;
        u->tx_shift_busy = false;
        u->tx_shift_reg = 0;

        if (u->tx_buffer_valid) {
            uart_start_tx_shift(u);
        }
    } else {
        u->tx_cycles_remaining -= cycles;
    }
}

void uart_receive_byte(UARTState *u, uint8_t byte)
{
    if (u->rx_buffer_full) {
        u->overrun = true;
    }

    u->rx_buffer = (uint16_t)byte | 0x0300u;
    u->rx_buffer_full = true;
    u->rxd_level = true;

    uart_raise_irq(u, UART_INTREQ_RBF);
}

void uart_clear_rbf(UARTState *u)
{
    u->rx_buffer_full = false;
    u->rx_buffer = 0;
    u->overrun = false;
}