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
    case UART_HOST_BACKEND_PL011: {
        bool ok = pl011_backend_write_byte(&host->pl011, byte);
        static int s_first_pl011_backend_attempt_logged = 0;
        static int s_first_pl011_backend_failure_logged = 0;

        if (!s_first_pl011_backend_attempt_logged) {
            s_first_pl011_backend_attempt_logged = 1;
            kprintf("[SERIAL-BRIDGE] first PL011 backend send byte=%02x open=%d enabled=%d ok=%d\n",
                    (unsigned)byte,
                    pl011_backend_is_open(&host->pl011) ? 1 : 0,
                    host->enabled ? 1 : 0,
                    ok ? 1 : 0);
        }

        if (!ok && !s_first_pl011_backend_failure_logged) {
            s_first_pl011_backend_failure_logged = 1;
            kprintf("[SERIAL-BRIDGE-FAIL] PL011 backend rejected byte=%02x open=%d enabled=%d\n",
                    (unsigned)byte,
                    pl011_backend_is_open(&host->pl011) ? 1 : 0,
                    host->enabled ? 1 : 0);
        }

        return ok;
    }

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
    case UART_HOST_BACKEND_PL011:
        return pl011_backend_read_byte(&host->pl011, byte_out);

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
    host->paula_serial = NULL;
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
    case UART_HOST_BACKEND_PL011:
        pl011_backend_close(&host->pl011);
        break;
    default:
        break;
    }

    host->backend_type = UART_HOST_BACKEND_NONE;
    host->enabled = false;
    host->paula_serial = NULL;
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
    PaulaSerial *attached_paula = NULL;

    if (!host) {
        return false;
    }

    attached_paula = host->paula_serial;
    uart_host_shutdown(host);

    if (!miniuart_backend_open(&host->mini_uart, baud)) {
        host->backend_type = UART_HOST_BACKEND_NONE;
        host->enabled = false;
        host->paula_serial = attached_paula;
        return false;
    }

    host->backend_type = UART_HOST_BACKEND_MINIUART;
    host->enabled = true;
    host->paula_serial = attached_paula;
    return true;
}

bool uart_host_open_pl011(UARTHost *host, uint32_t baud)
{
    PaulaSerial *attached_paula = NULL;

    if (!host) {
        return false;
    }

    attached_paula = host->paula_serial;
    uart_host_shutdown(host);

    if (!pl011_backend_open(&host->pl011, baud)) {
        host->backend_type = UART_HOST_BACKEND_NONE;
        host->enabled = false;
        host->paula_serial = attached_paula;
        return false;
    }

    host->backend_type = UART_HOST_BACKEND_PL011;
    host->enabled = true;
    host->paula_serial = attached_paula;
    return true;
}

void uart_host_attach_paula(UARTHost *host, PaulaSerial *paula_serial)
{
    if (!host) {
        return;
    }

    host->paula_serial = paula_serial;
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
#if defined(BELLATRIX_USE_RIGEL_CHIPSET) && BELLATRIX_USE_RIGEL_CHIPSET
    (void)host;
#else
    if (!host || !host->enabled || !host->paula_serial) {
        return;
    }

    uint8_t byte;

    while (uart_host_receive_byte(host, &byte)) {
        paula_serial_receive_byte(host->paula_serial, byte);
    }

    while (paula_serial_tx_available(host->paula_serial)) {
        if (!paula_serial_peek_tx_byte(host->paula_serial, &byte)) {
            break;
        }

        if (host->backend_type == UART_HOST_BACKEND_PL011) {
            static int s_first_pl011_tx_drain_logged = 0;
            if (!s_first_pl011_tx_drain_logged) {
                s_first_pl011_tx_drain_logged = 1;
                kprintf("[SERIAL-BRIDGE] draining Paula TX byte=%02x\n",
                        (unsigned)byte);
            }
        }

        if (!uart_host_send_byte(host, byte)) {
            break;
        }

        if (!paula_serial_pop_tx_byte(host->paula_serial, &byte)) {
            break;
        }

        if (host->backend_type == UART_HOST_BACKEND_PL011) {
            static int s_first_pl011_tx_logged = 0;
            if (!s_first_pl011_tx_logged) {
                s_first_pl011_tx_logged = 1;
                kprintf("[SERIAL-BRIDGE] first PL011 TX byte=%02x\n",
                        (unsigned)byte);
            }
        }

        if (host->backend_type == UART_HOST_BACKEND_PL011 &&
            kprintf_get_enabled())
        {
            kprintf_set_enabled(0);
        }
    }
#endif
}
