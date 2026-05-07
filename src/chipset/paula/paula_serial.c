#include "chipset/paula/paula_serial.h"

#include <string.h>

static uint32_t paula_serial_cycles_per_bit(const PaulaSerial *s)
{
    uint32_t v = (uint32_t)(s->serper & 0x7fffu) + 1u;
    return v ? v : 1u;
}

static uint32_t paula_serial_frame_cycles(const PaulaSerial *s, uint16_t word)
{
    (void)word;
    return 10u * paula_serial_cycles_per_bit(s);
}

static void paula_serial_raise_irq(PaulaSerial *s, uint16_t mask)
{
    if (s->irq_raise_cb) {
        s->irq_raise_cb(s->opaque, mask);
    }
}

static void paula_serial_queue_tx_byte(PaulaSerial *s, uint16_t word)
{
    uint8_t byte = (uint8_t)(word & 0xffu);

    if (s->tx_fifo_count >= PAULA_SERIAL_TX_FIFO_SIZE) {
        s->tx_overflow = true;
        return;
    }

    s->tx_fifo[s->tx_fifo_tail] = byte;
    s->tx_fifo_tail = (s->tx_fifo_tail + 1u) % PAULA_SERIAL_TX_FIFO_SIZE;
    s->tx_fifo_count++;
}

static void paula_serial_start_tx_shift(PaulaSerial *s)
{
    if (!s->tx_buffer_valid || s->tx_shift_busy) {
        return;
    }

    s->tx_shift_reg = s->tx_buffer;
    s->tx_shift_busy = true;
    s->tx_cycles_remaining = paula_serial_frame_cycles(s, s->tx_shift_reg);
    s->tx_buffer_valid = false;

    if (s->tx_instant) {
        paula_serial_queue_tx_byte(s, s->tx_shift_reg);
    }

    paula_serial_raise_irq(s, PAULA_SERIAL_INTREQ_TBE);
}

void paula_serial_init(PaulaSerial *s, void *opaque,
                       paula_serial_irq_raise_cb irq_raise_cb)
{
    if (!s) {
        return;
    }

    memset(s, 0, sizeof(*s));
    s->opaque = opaque;
    s->irq_raise_cb = irq_raise_cb;

    paula_serial_reset(s);
}

void paula_serial_reset(PaulaSerial *s)
{
    if (!s) {
        return;
    }

    s->serper = 0;

    s->tx_buffer = 0;
    s->tx_buffer_valid = false;

    s->tx_shift_reg = 0;
    s->tx_shift_busy = false;
    s->tx_cycles_remaining = 0;

    s->rx_buffer = 0;
    s->rx_buffer_full = false;
    s->overrun = false;
    s->rxd_level = true;

    s->tx_instant = true;

    s->tx_fifo_head = 0;
    s->tx_fifo_tail = 0;
    s->tx_fifo_count = 0;
    s->tx_overflow = false;
}

void paula_serial_write_serdat(PaulaSerial *s, uint16_t value)
{
    if (!s) {
        return;
    }

    if (s->tx_instant) {
        paula_serial_queue_tx_byte(s, value);
        paula_serial_raise_irq(s, PAULA_SERIAL_INTREQ_TBE);
        return;
    }

    s->tx_buffer = value;
    s->tx_buffer_valid = true;

    if (!s->tx_shift_busy) {
        paula_serial_start_tx_shift(s);
    }
}

void paula_serial_write_serper(PaulaSerial *s, uint16_t value)
{
    if (!s) {
        return;
    }

    s->serper = value;
}

uint16_t paula_serial_read_serdatr(const PaulaSerial *s)
{
    if (!s) {
        return 0xffffu;
    }

    uint16_t v = 0;

    v |= (s->rx_buffer & 0x03ffu);

    if (s->overrun)          v |= 0x8000u;
    if (s->rx_buffer_full)   v |= 0x4000u;
    if (!s->tx_buffer_valid) v |= 0x2000u;
    if (!s->tx_shift_busy)   v |= 0x1000u;
    if (s->rxd_level)        v |= 0x0800u;

    return v;
}

void paula_serial_step(PaulaSerial *s, uint32_t cycles)
{
    if (!s) {
        return;
    }

    if (!s->tx_shift_busy) {
        if (s->tx_buffer_valid) {
            paula_serial_start_tx_shift(s);
        }
        return;
    }

    if (cycles >= s->tx_cycles_remaining) {
        uint16_t completed_word = s->tx_shift_reg;

        s->tx_cycles_remaining = 0;
        s->tx_shift_busy = false;
        s->tx_shift_reg = 0;

        if (!s->tx_instant) {
            paula_serial_queue_tx_byte(s, completed_word);
        }

        if (s->tx_buffer_valid) {
            paula_serial_start_tx_shift(s);
        }
    } else {
        s->tx_cycles_remaining -= cycles;
    }
}

void paula_serial_receive_byte(PaulaSerial *s, uint8_t byte)
{
    if (!s) {
        return;
    }

    if (s->rx_buffer_full) {
        s->overrun = true;
    }

    s->rx_buffer = (uint16_t)byte | 0x0100u;
    s->rx_buffer_full = true;
    s->rxd_level = true;

    paula_serial_raise_irq(s, PAULA_SERIAL_INTREQ_RBF);
}

void paula_serial_clear_rbf(PaulaSerial *s)
{
    if (!s) {
        return;
    }

    s->rx_buffer_full = false;
    s->rx_buffer = 0;
    s->overrun = false;
}

bool paula_serial_tx_available(const PaulaSerial *s)
{
    return s && s->tx_fifo_count > 0;
}

bool paula_serial_peek_tx_byte(const PaulaSerial *s, uint8_t *byte_out)
{
    if (!s || !byte_out || s->tx_fifo_count == 0) {
        return false;
    }

    *byte_out = s->tx_fifo[s->tx_fifo_head];
    return true;
}

bool paula_serial_pop_tx_byte(PaulaSerial *s, uint8_t *byte_out)
{
    if (!s || !byte_out || s->tx_fifo_count == 0) {
        return false;
    }

    *byte_out = s->tx_fifo[s->tx_fifo_head];
    s->tx_fifo_head = (s->tx_fifo_head + 1u) % PAULA_SERIAL_TX_FIFO_SIZE;
    s->tx_fifo_count--;

    return true;
}

void paula_serial_set_tx_instant(PaulaSerial *s, bool enabled)
{
    if (!s) {
        return;
    }

    s->tx_instant = enabled;
}
