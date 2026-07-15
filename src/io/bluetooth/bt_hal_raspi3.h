#ifndef BELLATRIX_IO_BLUETOOTH_BT_HAL_RASPI3_H
#define BELLATRIX_IO_BLUETOOTH_BT_HAL_RASPI3_H

#include <stdint.h>

void bt_hal_raspi3_drain_fifo(void);
void bt_hal_raspi3_flush_rx(void);
void bt_hal_raspi3_poll_uart(void);
uint32_t bt_hal_raspi3_io_activity(void);
uint32_t bt_hal_raspi3_io_tx(void);
uint32_t bt_hal_raspi3_io_rx(void);
void bt_hal_raspi3_rx_pending(uint32_t *filled, uint32_t *wanted);
uint32_t bt_hal_raspi3_rx_ring_used(void);

/* Bounded UART0 top half. Never enters BTstack or performs logging. */
void bt_hal_raspi3_irq_rx(void);

uint32_t bt_hal_raspi3_irq_rx_bytes(void);
uint32_t bt_hal_raspi3_irq_rx_budget_hits(void);
uint32_t bt_hal_raspi3_rx_overflow(void);
uint32_t bt_hal_raspi3_rx_high_water(void);
uint32_t bt_hal_raspi3_uart_rx_errors(void);
uint32_t bt_hal_raspi3_uart_rx_framing_errors(void);
uint32_t bt_hal_raspi3_uart_rx_parity_errors(void);
uint32_t bt_hal_raspi3_uart_rx_break_errors(void);
uint32_t bt_hal_raspi3_uart_rx_overrun_errors(void);
uint32_t bt_hal_raspi3_reactor_rx_bytes(void);
uint32_t bt_hal_raspi3_reactor_rx_completions(void);
uint32_t bt_hal_raspi3_reactor_rx_budget_hits(void);

#endif
