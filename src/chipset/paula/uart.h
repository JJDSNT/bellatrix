#ifndef BELLATRIX_CHIPSET_PAULA_UART_H
#define BELLATRIX_CHIPSET_PAULA_UART_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*uart_tx_byte_cb)(void *opaque, uint8_t byte);
typedef void (*uart_irq_raise_cb)(void *opaque, uint16_t mask);

typedef enum UARTLinkMode {
    /* Backend representa a outra ponta do cabo:
     * Amiga SERDAT TX -> backend/parceiro RX
     * Amiga SERDATR RX <- backend/parceiro TX */
    UART_LINK_NULL_MODEM = 0,

    /* Backend já expõe o lado fisico cruzado e quer apenas observar/
     * encaminhar o fluxo sem cruzamento adicional no software. */
    UART_LINK_STRAIGHT_THROUGH = 1,
} UARTLinkMode;

typedef struct UARTState {
    void *opaque;
    uart_tx_byte_cb tx_cb;
    uart_irq_raise_cb irq_raise_cb;

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
    UARTLinkMode link_mode;
} UARTState;

/* Paula INTREQ bits relevantes */
#define UART_INTREQ_TBE  (1u << 0)
#define UART_INTREQ_RBF  (1u << 11)

void uart_init(UARTState *u, void *opaque,
               uart_tx_byte_cb tx_cb,
               uart_irq_raise_cb irq_raise_cb);

void uart_reset(UARTState *u);
void uart_set_link_mode(UARTState *u, UARTLinkMode mode);
UARTLinkMode uart_link_mode(const UARTState *u);

void uart_write_serdat(UARTState *u, uint16_t value);
void uart_write_serper(UARTState *u, uint16_t value);
uint16_t uart_read_serdatr(const UARTState *u);

void uart_step(UARTState *u, uint32_t cycles);

void uart_receive_byte(UARTState *u, uint8_t byte);
void uart_clear_rbf(UARTState *u);

static inline bool uart_tbe(const UARTState *u)
{
    return !u->tx_buffer_valid;
}

static inline bool uart_tsre(const UARTState *u)
{
    return !u->tx_shift_busy;
}

#ifdef __cplusplus
}
#endif

#endif
