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
    uint8_t byte = (uint8_t)(word & 0xFFu);

    switch (u->link_mode) {
    case UART_LINK_STRAIGHT_THROUGH:
        /* Straight-through backends already model the physical crossing
         * outside the emulator, so SERDAT still leaves Bellatrix here. */
        break;
    case UART_LINK_NULL_MODEM:
    default:
        /* Null-modem backends represent the remote endpoint directly:
         * SERDAT TX must be delivered to the partner RX. */
        break;
    }

    if (u->tx_cb) {
        u->tx_cb(u->opaque, byte);
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
    u->link_mode = UART_LINK_NULL_MODEM;
}

void uart_set_link_mode(UARTState *u, UARTLinkMode mode)
{
    u->link_mode = mode;
}

UARTLinkMode uart_link_mode(const UARTState *u)
{
    return u->link_mode;
}

void uart_write_serdat(UARTState *u, uint16_t value)
{
    if (u->tx_instant) {
        /* No shift-register timing: emit byte and signal TBE immediately.
         * tx_buffer_valid and tx_shift_busy stay false, so SERDATR always
         * reports TBE=1 / TSRE=1. */
        uart_emit_tx_byte(u, value);
        uart_raise_irq(u, UART_INTREQ_TBE);
        return;
    }

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

    /* Present incoming data in the same 9-bit shape expected by Paula
     * software paths that sample SERDATR directly. */
    u->rx_buffer = (uint16_t)byte | 0x0100u;
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
