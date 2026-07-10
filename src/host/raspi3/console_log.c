#include "host/raspi3/console_log.h"

#include "host/raspi3/pl011_backend.h"
#include "io/serial/miniuart_backend.h"
#include "support.h"

#include <stdatomic.h>
#include <stdint.h>

/* Added to Emu68's support_rpi.c by patches/0008-bellatrix-console-redirect.patch. */
void kprintf_set_putc_override(void (*fn)(char chr));

static MiniUartBackend s_console_miniuart;

#define CONSOLE_LOG_RING_SIZE 4096u
#define CONSOLE_LOG_DRAIN_MAX 256u
static volatile uint8_t  s_ring[CONSOLE_LOG_RING_SIZE];
static _Atomic uint32_t s_ring_head;    /* producer: console_log_putc()  */
static _Atomic uint32_t s_ring_tail;    /* consumer: console_log_drain() */
static _Atomic uint32_t s_ring_dropped;
static volatile uint32_t s_direct_mode;

static void ring_push(uint8_t byte)
{
    uint32_t head = atomic_load_explicit(&s_ring_head, memory_order_relaxed);
    uint32_t next = (head + 1u) % CONSOLE_LOG_RING_SIZE;
    uint32_t tail = atomic_load_explicit(&s_ring_tail, memory_order_acquire);

    if (next == tail) {
        atomic_fetch_add_explicit(&s_ring_dropped, 1u, memory_order_relaxed);
        return;
    }

    /* Publish head only after the payload is visible.  `volatile` alone did
     * not provide this ordering on AArch64, so the Core 2 consumer could read
     * stale bytes left from a previous ring revolution and replay whole old
     * log blocks. */
    s_ring[head] = byte;
    atomic_store_explicit(&s_ring_head, next, memory_order_release);
}

/* Multicore-friendly sink: kprintf calls putc one char at a time, so when
 * several cores log concurrently their characters interleave and garble the
 * output (and ring_push()'s head RMW races). Buffer each core's current line
 * separately and flush the whole line under a lock, so lines stay intact and
 * only one core pushes to the ring at a time. Line granularity (newline is the
 * boundary) avoids having to hook kprintf's call boundary. */
#define CONSOLE_LOG_LINE_MAX 256u
static char     s_line[4][CONSOLE_LOG_LINE_MAX];
static uint32_t s_line_len[4];
static volatile unsigned char s_sink_lock;

static inline unsigned console_this_core(void)
{
    uint64_t mpidr;
    __asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(mpidr));
    return (unsigned)(mpidr & 3u);
}

static void console_log_putc(char chr)
{
    if (__atomic_load_n(&s_direct_mode, __ATOMIC_ACQUIRE)) {
        /* Early boot: only Core 0 runs, so per-char immediate output is safe
         * and keeps a mid-line hang's last partial line visible. */
        if (chr == '\n') {
            int spin = 1000000;
            while (!miniuart_backend_write_byte(&s_console_miniuart, (uint8_t)'\r') &&
                   --spin > 0) {
            }
        }

        int spin = 1000000;
        while (!miniuart_backend_write_byte(&s_console_miniuart, (uint8_t)chr) &&
               --spin > 0) {
        }
        return;
    }

    /* Runtime: any of the 4 cores may log. Accumulate this core's line, flush
     * the whole thing atomically on newline (or when the buffer fills). */
    unsigned core = console_this_core();

    if (s_line_len[core] < CONSOLE_LOG_LINE_MAX)
        s_line[core][s_line_len[core]++] = chr;

    if (chr != '\n' && s_line_len[core] < CONSOLE_LOG_LINE_MAX)
        return;

    while (__atomic_test_and_set(&s_sink_lock, __ATOMIC_ACQUIRE))
        __asm__ volatile("yield");

    for (uint32_t i = 0u; i < s_line_len[core]; i++) {
        uint8_t c = (uint8_t)s_line[core][i];
        if (c == (uint8_t)'\n')
            ring_push((uint8_t)'\r');
        ring_push(c);
    }
    s_line_len[core] = 0u;

    __atomic_clear(&s_sink_lock, __ATOMIC_RELEASE);
}

void console_log_drain(void)
{
    uint32_t drained = 0u;
    uint32_t tail;

    if (!miniuart_backend_is_open(&s_console_miniuart))
        return;

    /* miniuart_backend_write_byte() now reports FIFO-full (false) instead of
     * overrunning the 8-byte AUX TX FIFO -- stop this pass on the first
     * failure and retry the same byte next call, rather than advancing the
     * tail and dropping/corrupting it. Cap each pass so logs stay
     * opportunistic. */
    tail = atomic_load_explicit(&s_ring_tail, memory_order_relaxed);
    while (tail != atomic_load_explicit(&s_ring_head, memory_order_acquire) &&
           drained < CONSOLE_LOG_DRAIN_MAX) {
        if (!miniuart_backend_write_byte(&s_console_miniuart,
                                          s_ring[tail]))
            break;
        tail = (tail + 1u) % CONSOLE_LOG_RING_SIZE;
        atomic_store_explicit(&s_ring_tail, tail, memory_order_release);
        drained++;
    }
}

void console_log_set_deferred(void)
{
    __atomic_store_n(&s_direct_mode, 0u, __ATOMIC_RELEASE);
}

void bellatrix_console_log_init_early(uint32_t core_hz)
{
    if (core_hz == 0u)
        core_hz = 250000000u;

    pl011_backend_route_header_to_miniuart();

    if (miniuart_backend_open_clk(&s_console_miniuart, 115200u, core_hz)) {
        __atomic_store_n(&s_direct_mode, 1u, __ATOMIC_RELEASE);
        kprintf_set_putc_override(console_log_putc);
        kprintf_set_enabled(1);
    }
}

void bellatrix_console_log_reclock(uint32_t core_hz)
{
    if (core_hz == 0u)
        return;
    miniuart_backend_set_baud_clk(&s_console_miniuart, 115200u, core_hz);
}
