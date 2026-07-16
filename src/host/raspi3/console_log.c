#include "host/raspi3/console_log.h"

#include "host/raspi3/pl011_backend.h"
#include "io/serial/miniuart_backend.h"
#include "support.h"

#include <stdatomic.h>
#include <stdint.h>

/* Added to Emu68's support_rpi.c by patches/0008-bellatrix-console-redirect.patch. */
void kprintf_set_putc_override(void (*fn)(char chr));

static MiniUartBackend s_console_miniuart;

#define CONSOLE_LOG_RING_SIZE 2048u
#define CONSOLE_LOG_RING_MASK (CONSOLE_LOG_RING_SIZE - 1u)
#define CONSOLE_LOG_DRAIN_MAX 256u

typedef struct ConsoleCoreRing {
    _Alignas(64) _Atomic uint32_t head;
    _Alignas(64) _Atomic uint32_t tail;
    _Alignas(64) uint8_t data[CONSOLE_LOG_RING_SIZE];
    _Atomic uint32_t dropped_lines;
} ConsoleCoreRing;

static ConsoleCoreRing s_rings[4];
static volatile uint32_t s_direct_mode;

static bool ring_push_line(unsigned core, const char *line, uint32_t length)
{
    ConsoleCoreRing *ring = &s_rings[core & 3u];
    uint32_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_acquire);
    uint32_t used = (head - tail) & CONSOLE_LOG_RING_MASK;
    uint32_t available = CONSOLE_LOG_RING_MASK - used;
    uint32_t needed = length;

    for (uint32_t i = 0u; i < length; i++) {
        if ((uint8_t)line[i] == (uint8_t)'\n')
            needed++;
    }

    if (needed > available) {
        atomic_fetch_add_explicit(&ring->dropped_lines, 1u,
                                  memory_order_relaxed);
        return false;
    }

    for (uint32_t i = 0u; i < length; i++) {
        uint8_t byte = (uint8_t)line[i];
        if (byte == (uint8_t)'\n') {
            ring->data[head] = (uint8_t)'\r';
            head = (head + 1u) & CONSOLE_LOG_RING_MASK;
        }
        ring->data[head] = byte;
        head = (head + 1u) & CONSOLE_LOG_RING_MASK;
    }

    /* Publish once after the complete line/chunk is visible. */
    atomic_store_explicit(&ring->head, head, memory_order_release);
    return true;
}

/* Multicore-friendly sink: kprintf calls putc one char at a time, so when
 * several cores log concurrently their characters interleave and garble the
 * output (and ring_push()'s head RMW races). Buffer each core's current line
 * separately and publish it to that core's SPSC ring. No producer contends on
 * a global lock; the host reactor preserves line boundaries while draining. */
#define CONSOLE_LOG_LINE_MAX 256u
static char     s_line[4][CONSOLE_LOG_LINE_MAX];
static uint32_t s_line_len[4];

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

    /* Reserve one byte for the newline. Overlong lines are truncated rather
     * than published as interleavable chunks. */
    if (chr != '\n') {
        if (s_line_len[core] < CONSOLE_LOG_LINE_MAX - 1u)
            s_line[core][s_line_len[core]++] = chr;
        return;
    }

    s_line[core][s_line_len[core]++] = chr;

    (void)ring_push_line(core, s_line[core], s_line_len[core]);
    s_line_len[core] = 0u;
}

void console_log_drain(void)
{
    static unsigned active_core = 4u;
    static unsigned next_core;
    uint32_t drained = 0u;

    if (!miniuart_backend_is_open(&s_console_miniuart))
        return;

    /* miniuart_backend_write_byte() now reports FIFO-full (false) instead of
     * overrunning the 8-byte AUX TX FIFO -- stop this pass on the first
     * failure and retry the same byte next call, rather than advancing the
     * tail and dropping/corrupting it. Cap each pass so logs stay
     * opportunistic. */
    while (drained < CONSOLE_LOG_DRAIN_MAX) {
        if (active_core >= 4u) {
            unsigned scanned;
            for (scanned = 0u; scanned < 4u; scanned++) {
                unsigned core = (next_core + scanned) & 3u;
                ConsoleCoreRing *ring = &s_rings[core];
                uint32_t tail = atomic_load_explicit(&ring->tail,
                                                     memory_order_relaxed);
                uint32_t head = atomic_load_explicit(&ring->head,
                                                     memory_order_acquire);
                if (tail != head) {
                    active_core = core;
                    break;
                }
            }
            if (active_core >= 4u)
                break;
        }

        ConsoleCoreRing *ring = &s_rings[active_core];
        uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
        uint32_t head = atomic_load_explicit(&ring->head, memory_order_acquire);

        if (tail == head) {
            /* Complete lines are published atomically. */
            active_core = 4u;
            continue;
        }

        uint8_t byte = ring->data[tail];
        if (!miniuart_backend_write_byte(&s_console_miniuart, byte))
            break;

        tail = (tail + 1u) & CONSOLE_LOG_RING_MASK;
        atomic_store_explicit(&ring->tail, tail, memory_order_release);
        drained++;

        if (byte == (uint8_t)'\n') {
            next_core = (active_core + 1u) & 3u;
            active_core = 4u;
        }
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
