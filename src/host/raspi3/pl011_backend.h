#ifndef BELLATRIX_HOST_RASPI3_PL011_BACKEND_H
#define BELLATRIX_HOST_RASPI3_PL011_BACKEND_H

#include <stdint.h>
#include <stdbool.h>

/*
 * PL011 (UART0) is owned by the Pi 3 on-board Bluetooth transport from the
 * beginning of boot. AUX miniUART independently owns logging on GPIO 14/15;
 * Bellatrix never hands the console from one UART to the other at runtime.
 */

typedef struct {
    bool     open;
    uint32_t baud;
} PL011Backend;

bool pl011_backend_open(PL011Backend *b, uint32_t baud);
/* flow=true enables hardware RTS/CTS (CR.RTSEn/CTSEn) — required for the
 * Pi 3 Bluetooth path at 921600 baud, where the 16-byte RX FIFO overruns
 * between polls; GPIO 30/31 must be routed ALT3 (route_bluetooth_pi3). */
bool pl011_backend_open_flow(PL011Backend *b, uint32_t baud, bool flow);
void pl011_backend_close(PL011Backend *b);
bool pl011_backend_is_open(const PL011Backend *b);

bool pl011_backend_read_byte(PL011Backend *b, uint8_t *byte_out);
void pl011_backend_rx_irq_enable(bool enable);
bool pl011_backend_write_byte(PL011Backend *b, uint8_t byte);
bool pl011_backend_route_header_console(void);
bool pl011_backend_route_bluetooth_pi3(void);
/* GPIO 14/15 -> ALT5 (mini-UART) only — no PL011/BT-pin changes. Used to
 * claim the header pins for kprintf's mini-UART console before BT ever
 * touches GPIO, so there is no runtime handoff to coordinate. */
bool pl011_backend_route_header_to_miniuart(void);
/* Program GPCLK2 → GPIO 43 as the BT 32.768 kHz LPO clock; returns the new
 * CM_GP2CTL value and reports the previous CTL/DIV through the pointers. */
uint32_t pl011_backend_setup_bt_lpo(uint32_t *old_ctl, uint32_t *old_div);
void pl011_backend_wait_idle(void);

#endif
