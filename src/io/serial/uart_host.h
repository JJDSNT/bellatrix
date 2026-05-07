#ifndef BELLATRIX_IO_SERIAL_UART_HOST_H
#define BELLATRIX_IO_SERIAL_UART_HOST_H

#include <stdint.h>
#include <stdbool.h>

#include "chipset/paula/paula_serial.h"
#include "io/serial/null_modem.h"
#include "io/serial/pty_backend.h"
#include "io/serial/miniuart_backend.h"
#include "host/raspi3/pl011_backend.h"

typedef enum UARTHostBackendType {
    UART_HOST_BACKEND_NONE = 0,
    UART_HOST_BACKEND_PTY,
    UART_HOST_BACKEND_MINIUART,
    UART_HOST_BACKEND_PL011
} UARTHostBackendType;

typedef struct UARTHost {
    UARTHostBackendType backend_type;

    PaulaSerial *paula_serial;

    PtyBackend pty;
    MiniUartBackend mini_uart;
    PL011Backend pl011;
    NullModem null_modem;

    bool enabled;
} UARTHost;

void uart_host_init(UARTHost *host);
void uart_host_shutdown(UARTHost *host);

bool uart_host_open_pty(UARTHost *host);
const char *uart_host_pty_name(const UARTHost *host);

bool uart_host_open_miniuart(UARTHost *host, uint32_t baud);
bool uart_host_open_pl011(UARTHost *host, uint32_t baud);

void uart_host_attach_paula(UARTHost *host, PaulaSerial *paula_serial);

void uart_host_set_null_modem_mode(UARTHost *host, NullModemMode mode);

void uart_host_poll(UARTHost *host);

bool uart_host_send_byte(UARTHost *host, uint8_t byte);
bool uart_host_receive_byte(UARTHost *host, uint8_t *byte_out);

#endif