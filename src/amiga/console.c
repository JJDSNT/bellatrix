#include "amiga/console.h"

#include "A64.h"

#include <stdatomic.h>
#include <stdint.h>

void kprintf_set_putc_override(void (*fn)(char chr));
void kprintf_raw_putc(char chr);

#define CONSOLE_RING_SIZE 4096u
#define CONSOLE_RING_MASK (CONSOLE_RING_SIZE - 1u)
#define CONSOLE_LINE_MAX  240u
#define CONSOLE_DRAIN_MAX 512u

/*
 * One ring per core, and each index on its own cache line.
 *
 * Legacy used one ring per core rather than a shared one because a shared
 * head is a read-modify-write between producers; per-core rings make each a
 * single-producer, single-consumer queue with no atomics beyond the two
 * indices. The alignment is not decoration: head and tail are written by
 * different cores, and sharing a line between them turns every publication
 * into a coherency round trip.
 */
typedef struct ConsoleRing
{
    _Alignas(64) _Atomic uint32_t head;
    _Alignas(64) _Atomic uint32_t tail;
    _Alignas(64) uint8_t data[CONSOLE_RING_SIZE];
    _Atomic uint32_t dropped;
} ConsoleRing;

static ConsoleRing rings[4];
static char line[4][CONSOLE_LINE_MAX];
static uint32_t line_len[4];
static volatile uint32_t console_ready;

static inline unsigned this_core(void)
{
    uint64_t mpidr;
    __asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(mpidr));
    return (unsigned)(mpidr & 3u);
}

static void ring_push(unsigned core, const char *src, uint32_t length)
{
    ConsoleRing *ring = &rings[core];
    uint32_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_acquire);
    uint32_t used = (head - tail) & CONSOLE_RING_MASK;
    uint32_t i;

    /*
     * Drop the line rather than wait for room. A console that blocks is the
     * thing this exists to remove, and a counter says what was lost -- which
     * is what a silent truncation does not.
     */
    if (length > CONSOLE_RING_MASK - used)
    {
        atomic_fetch_add_explicit(&ring->dropped, 1u, memory_order_relaxed);
        return;
    }

    for (i = 0; i < length; i++)
    {
        ring->data[head] = (uint8_t)src[i];
        head = (head + 1u) & CONSOLE_RING_MASK;
    }

    /* Publish once, after the whole line is in memory. */
    atomic_store_explicit(&ring->head, head, memory_order_release);
}

static void console_putc(char chr)
{
    unsigned core = this_core();

    /* putByte inserts the carriage return itself; adding one here doubles it. */
    if (line_len[core] < CONSOLE_LINE_MAX)
        line[core][line_len[core]++] = chr;

    if (chr != '\n' && line_len[core] < CONSOLE_LINE_MAX)
        return;

    ring_push(core, line[core], line_len[core]);
    line_len[core] = 0;
}

void amiga_console_init(void)
{
    console_ready = 1;
    /*
     * Wake the drainer. Without this it stays in the WFE below and nothing
     * empties the rings, so every line after this call is written into memory
     * and never reaches the wire -- the machine runs and the log stops dead.
     *
     * It went unnoticed because another core's enable happened to send an
     * event just afterwards, so the drainer woke by accident; the build with
     * the chipset core disabled has no such accident and the log ended at the
     * last line printed before this function.
     */
    __asm__ volatile("dsb ishst\n\tsev" ::: "memory");
    kprintf_set_putc_override(console_putc);
}

void amiga_console_drain(void)
{
    static unsigned next_core;
    uint32_t drained = 0;
    unsigned scanned;

    while (drained < CONSOLE_DRAIN_MAX)
    {
        ConsoleRing *ring = 0;
        unsigned core = 0;

        for (scanned = 0; scanned < 4u; scanned++)
        {
            unsigned c = (next_core + scanned) & 3u;
            ConsoleRing *r = &rings[c];

            if (atomic_load_explicit(&r->tail, memory_order_relaxed) !=
                atomic_load_explicit(&r->head, memory_order_acquire))
            {
                ring = r;
                core = c;
                break;
            }
        }
        if (ring == 0)
            return;

        {
            uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
            uint8_t byte = ring->data[tail];

            kprintf_raw_putc((char)byte);
            atomic_store_explicit(&ring->tail, (tail + 1u) & CONSOLE_RING_MASK,
                                  memory_order_release);
            drained++;

            /*
             * Round-robin at line boundaries, not per byte: a core that is
             * logging heavily must not lock another out, and a line must not
             * be cut to be fair.
             */
            if (byte == (uint8_t)'\n')
                next_core = (core + 1u) & 3u;
        }
    }
}

void amiga_console_run_on_core(void)
{
    uint64_t id;

    __asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(id));
    id &= 3;

    /*
     * Spin rather than sleep. This core has nothing else to do, and a missed
     * event here costs the whole log -- which is precisely what happened. WFE
     * is also a no-op under QEMU, so a lost wakeup is invisible until
     * hardware, and that is not a class of bug worth keeping for the sake of
     * a parked core's power.
     */
    while (!console_ready)
        __asm__ volatile("yield" ::: "memory");

    kprintf("[BELLATRIX:CONSOLE] core %u drains the console\n", (unsigned)id);

    for (;;)
    {
        amiga_console_drain();
        __asm__ volatile("yield" ::: "memory");
    }
}
