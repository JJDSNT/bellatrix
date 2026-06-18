#include "host/raspi3/console_log.h"

#include "host/raspi3/vc_mailbox.h"
#include "host/raspi3/pl011_backend.h"
#include "io/serial/miniuart_backend.h"
#include "support.h"

#include <stdint.h>

/* Added to Emu68's support_rpi.c by patches/0008-bellatrix-console-redirect.patch. */
void kprintf_set_putc_override(void (*fn)(char chr));

static MiniUartBackend s_console_miniuart;

#define CONSOLE_LOG_RING_SIZE 4096u
#define CONSOLE_LOG_DRAIN_MAX 256u
static volatile uint8_t  s_ring[CONSOLE_LOG_RING_SIZE];
static volatile uint32_t s_ring_head;    /* producer: console_log_putc()  */
static volatile uint32_t s_ring_tail;    /* consumer: console_log_drain() */
static volatile uint32_t s_ring_dropped;

static void ring_push(uint8_t byte)
{
    uint32_t head = s_ring_head;
    uint32_t next = (head + 1u) % CONSOLE_LOG_RING_SIZE;
    if (next == s_ring_tail) {
        s_ring_dropped++;
        return;
    }
    s_ring[head] = byte;
    s_ring_head = next;
}

static void console_log_putc(char chr)
{
    if (chr == '\n')
        ring_push((uint8_t)'\r');
    ring_push((uint8_t)chr);
}

void console_log_drain(void)
{
    uint32_t drained = 0u;

    if (!miniuart_backend_is_open(&s_console_miniuart))
        return;

    /* miniuart_backend_write_byte() is intentionally non-blocking for the
     * Bellatrix step cadence.  Do not gate this on LSR TX-ready: QEMU's AUX
     * UART status bit is not reliable, and a false zero would leave the log
     * ring permanently undrained.  Cap each pass so logs stay opportunistic. */
    while (s_ring_tail != s_ring_head && drained < CONSOLE_LOG_DRAIN_MAX) {
        miniuart_backend_write_byte(&s_console_miniuart,
                                    s_ring[s_ring_tail]);
        s_ring_tail = (s_ring_tail + 1u) % CONSOLE_LOG_RING_SIZE;
        drained++;
    }
}

void console_log_init(void)
{
    uint32_t core_hz = vc_get_core_clock_hz();
    if (core_hz == 0u)
        core_hz = 250000000u;

    pl011_backend_route_header_to_miniuart();

    /* 9600: matches Paula's existing host-side baud (uart_host_open_miniuart()
     * in bellatrix.c) — both consumers share one physical wire, so an
     * open() call from either side must target the same nominal rate. */
    if (miniuart_backend_open_clk(&s_console_miniuart, 9600u, core_hz))
        kprintf_set_putc_override(console_log_putc);
    else
        kprintf_set_enabled(0);
}
