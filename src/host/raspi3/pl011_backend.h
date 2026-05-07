#ifndef BELLATRIX_HOST_RASPI3_PL011_BACKEND_H
#define BELLATRIX_HOST_RASPI3_PL011_BACKEND_H

#include <stdint.h>
#include <stdbool.h>

/*
 * PL011 (UART0) as the Amiga serial backend.
 *
 * On Raspberry Pi 3B, UART0 (PL011) sits on GPIO 14/15 — the same header
 * pins that carry kprintf in a normal Bellatrix build.  When this backend is
 * opened, the PL011 is re-initialised at a known host-side baud rate under
 * Bellatrix's control; kprintf continues to fire the same registers, so boot
 * messages still reach the terminal. Once Kickstart/DiagROM runs, kprintf
 * goes quiet and the terminal becomes a clean bidirectional Amiga serial
 * console.
 *
 * Paula's emulated SERPER timing is handled inside Bellatrix and does not
 * need to match the physical USB-TTL baud. Default host baud: 115200
 * (match PuTTY/screen settings and keep boot logs readable).
 * Wire: USB-TTL adapter on GPIO 14 (TXD, pin 8) and GPIO 15 (RXD, pin 10).
 */

typedef struct {
    bool     open;
    uint32_t baud;
} PL011Backend;

bool pl011_backend_open(PL011Backend *b, uint32_t baud);
void pl011_backend_close(PL011Backend *b);
bool pl011_backend_is_open(const PL011Backend *b);

bool pl011_backend_read_byte(PL011Backend *b, uint8_t *byte_out);
bool pl011_backend_write_byte(PL011Backend *b, uint8_t byte);

#endif
