#include "io/serial/uart_host.h"

#include "support.h"

#include <string.h>

static bool uart_host_backend_send(void *opaque, uint8_t byte)
{
    UARTHost *host = (UARTHost *)opaque;

    if (!host || !host->enabled) {
        return false;
    }

    switch (host->backend_type) {
#if defined(BELLATRIX_ENABLE_PTY_BACKEND)
    case UART_HOST_BACKEND_PTY:
        return pty_backend_write_byte(&host->pty, byte);
#endif
    case UART_HOST_BACKEND_MINIUART:
        return miniuart_backend_write_byte(&host->mini_uart, byte);

    case UART_HOST_BACKEND_NONE:
    default:
        return false;
    }
}

static bool uart_host_backend_recv(void *opaque, uint8_t *byte_out)
{
    UARTHost *host = (UARTHost *)opaque;

    if (!host || !host->enabled || !byte_out) {
        return false;
    }

    switch (host->backend_type) {
#if defined(BELLATRIX_ENABLE_PTY_BACKEND)
    case UART_HOST_BACKEND_PTY:
        return pty_backend_read_byte(&host->pty, byte_out);
#endif
    case UART_HOST_BACKEND_MINIUART:
        return miniuart_backend_read_byte(&host->mini_uart, byte_out);

    case UART_HOST_BACKEND_NONE:
    default:
        return false;
    }
}

void uart_host_init(UARTHost *host)
{
    if (!host) {
        return;
    }

    memset(host, 0, sizeof(*host));

    host->backend_type = UART_HOST_BACKEND_NONE;
    host->enabled = false;

    null_modem_init(&host->null_modem,
                    host,
                    uart_host_backend_send,
                    uart_host_backend_recv);
}

void uart_host_shutdown(UARTHost *host)
{
    if (!host) {
        return;
    }

    switch (host->backend_type) {
#if defined(BELLATRIX_ENABLE_PTY_BACKEND)
    case UART_HOST_BACKEND_PTY:
        pty_backend_close(&host->pty);
        break;
#endif
    case UART_HOST_BACKEND_MINIUART:
        miniuart_backend_close(&host->mini_uart);
        break;
    default:
        break;
    }

    host->backend_type = UART_HOST_BACKEND_NONE;
    host->enabled = false;
}

bool uart_host_open_pty(UARTHost *host)
{
    if (!host) {
        return false;
    }

    if (
#if defined(BELLATRIX_ENABLE_PTY_BACKEND)
        host->backend_type == UART_HOST_BACKEND_PTY
#else
        false
#endif
    ) {
        pty_backend_close(&host->pty);
    }

#if !defined(BELLATRIX_ENABLE_PTY_BACKEND)
    host->backend_type = UART_HOST_BACKEND_NONE;
    host->enabled = false;
    return false;
#else
    if (!pty_backend_open(&host->pty)) {
        host->backend_type = UART_HOST_BACKEND_NONE;
        host->enabled = false;
        return false;
    }

    host->backend_type = UART_HOST_BACKEND_PTY;
    host->enabled = true;

    return true;
#endif
}

const char *uart_host_pty_name(const UARTHost *host)
{
#if !defined(BELLATRIX_ENABLE_PTY_BACKEND)
    (void)host;
    return NULL;
#else
    if (!host || host->backend_type != UART_HOST_BACKEND_PTY) {
        return NULL;
    }

    return pty_backend_slave_name(&host->pty);
#endif
}

bool uart_host_open_miniuart(UARTHost *host, uint32_t baud)
{
    if (!host) {
        return false;
    }

    uart_host_shutdown(host);

    if (!miniuart_backend_open(&host->mini_uart, baud)) {
        host->backend_type = UART_HOST_BACKEND_NONE;
        host->enabled = false;
        return false;
    }

    host->backend_type = UART_HOST_BACKEND_MINIUART;
    host->enabled = true;
    return true;
}

void uart_host_set_null_modem_mode(UARTHost *host, NullModemMode mode)
{
    if (!host) {
        return;
    }

    null_modem_set_mode(&host->null_modem, mode);
}

bool uart_host_send_byte(UARTHost *host, uint8_t byte)
{
    if (!host || !host->enabled) {
        return false;
    }

    return null_modem_send_from_amiga(&host->null_modem, byte);
}

bool uart_host_receive_byte(UARTHost *host, uint8_t *byte_out)
{
    if (!host || !host->enabled || !byte_out) {
        return false;
    }

    return null_modem_receive_to_amiga(&host->null_modem, byte_out);
}

void uart_host_poll(UARTHost *host)
{
    (void)host;
}
