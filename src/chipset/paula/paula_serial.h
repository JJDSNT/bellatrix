#ifndef BELLATRIX_PAULA_SERIAL_H
#define BELLATRIX_PAULA_SERIAL_H

#include <stdint.h>
#include <stdbool.h>

#define PAULA_SERIAL_INTREQ_RBF 0x0800u
#define PAULA_SERIAL_INTREQ_TBE 0x0001u

#define PAULA_SERIAL_TX_FIFO_SIZE 16

typedef void (*paula_serial_irq_raise_cb)(void *opaque, uint16_t mask);

typedef struct PaulaSerial {
    void *opaque;
    paula_serial_irq_raise_cb irq_raise_cb;

    uint16_t serper;

    uint16_t tx_buffer;
    bool tx_buffer_valid;

    uint16_t tx_shift_reg;
    bool tx_shift_busy;
    uint32_t tx_cycles_remaining;

    uint16_t rx_buffer;
    bool rx_buffer_full;
    bool overrun;
    bool rxd_level;

    bool tx_instant;

    uint8_t tx_fifo[PAULA_SERIAL_TX_FIFO_SIZE];
    unsigned tx_fifo_head;
    unsigned tx_fifo_tail;
    unsigned tx_fifo_count;
    bool tx_overflow;
} PaulaSerial;

void paula_serial_init(PaulaSerial *s, void *opaque,
                       paula_serial_irq_raise_cb irq_raise_cb);

void paula_serial_reset(PaulaSerial *s);

void paula_serial_write_serdat(PaulaSerial *s, uint16_t value);
void paula_serial_write_serper(PaulaSerial *s, uint16_t value);

uint16_t paula_serial_read_serdatr(const PaulaSerial *s);

void paula_serial_step(PaulaSerial *s, uint32_t cycles);

void paula_serial_receive_byte(PaulaSerial *s, uint8_t byte);
void paula_serial_clear_rbf(PaulaSerial *s);

bool paula_serial_tx_available(const PaulaSerial *s);
bool paula_serial_peek_tx_byte(const PaulaSerial *s, uint8_t *byte_out);
bool paula_serial_pop_tx_byte(PaulaSerial *s, uint8_t *byte_out);

void paula_serial_set_tx_instant(PaulaSerial *s, bool enabled);

#endif
