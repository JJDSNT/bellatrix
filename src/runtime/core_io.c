// src/runtime/core_io.c
//
// Core 3 — Physical peripherals domain.
//
// Owns: USB host stack, Bluetooth host stack, physical UART and console drain.
// Core 2 remains the sole owner of Rigel/Paula and exchanges serial bytes with
// this core through two SPSC queues.

#include "runtime/core_io.h"
#include "runtime/runtime.h"

#include <stdatomic.h>
#include <string.h>

#include "io/usb/usb_host.h"
#include "debug/core_log.h"
#include "host/raspi3/console_log.h"

#define CORE_IO_SERIAL_QUEUE_SIZE 1024u
#define CORE_IO_SERIAL_QUEUE_MASK (CORE_IO_SERIAL_QUEUE_SIZE - 1u)

typedef struct CoreIOSerialQueue {
    _Alignas(64) _Atomic uint32_t head;
    _Alignas(64) _Atomic uint32_t tail;
    _Alignas(64) uint8_t data[CORE_IO_SERIAL_QUEUE_SIZE];
    _Atomic uint32_t dropped;
    _Atomic uint32_t max_depth;
} CoreIOSerialQueue;

static CoreIOSerialQueue s_serial_tx;
static CoreIOSerialQueue s_serial_rx;

static uint32_t serial_queue_depth(const CoreIOSerialQueue *q)
{
    uint32_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    return (head - tail) & CORE_IO_SERIAL_QUEUE_MASK;
}

static bool serial_queue_push(CoreIOSerialQueue *q, uint8_t byte)
{
    uint32_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t next = (head + 1u) & CORE_IO_SERIAL_QUEUE_MASK;
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);

    if (next == tail) {
        atomic_fetch_add_explicit(&q->dropped, 1u, memory_order_relaxed);
        return false;
    }

    q->data[head] = byte;
    atomic_store_explicit(&q->head, next, memory_order_release);

    uint32_t depth = (next - tail) & CORE_IO_SERIAL_QUEUE_MASK;
    uint32_t old_max = atomic_load_explicit(&q->max_depth, memory_order_relaxed);
    while (depth > old_max &&
           !atomic_compare_exchange_weak_explicit(&q->max_depth, &old_max, depth,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
    }
    return true;
}

static bool serial_queue_peek(CoreIOSerialQueue *q, uint8_t *byte_out)
{
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    if (tail == head)
        return false;
    *byte_out = q->data[tail];
    return true;
}

static void serial_queue_consume(CoreIOSerialQueue *q)
{
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    atomic_store_explicit(&q->tail, (tail + 1u) & CORE_IO_SERIAL_QUEUE_MASK,
                          memory_order_release);
}

static bool serial_queue_pop(CoreIOSerialQueue *q, uint8_t *byte_out)
{
    if (!byte_out || !serial_queue_peek(q, byte_out))
        return false;
    serial_queue_consume(q);
    return true;
}

bool core_io_serial_enqueue_tx(uint8_t byte)
{
    bool queued = serial_queue_push(&s_serial_tx, byte);
#if defined(__aarch64__) && defined(BELLATRIX_ENABLE_MULTICORE)
    if (queued)
        __asm__ volatile("dsb ishst\n\tsev" ::: "memory");
#endif
    return queued;
}

bool core_io_serial_dequeue_rx(uint8_t *byte_out)
{
    return serial_queue_pop(&s_serial_rx, byte_out);
}

void core_io_serial_get_stats(CoreIOSerialStats *stats_out)
{
    if (!stats_out)
        return;
    stats_out->tx_depth = serial_queue_depth(&s_serial_tx);
    stats_out->tx_max_depth = atomic_load_explicit(&s_serial_tx.max_depth,
                                                   memory_order_relaxed);
    stats_out->tx_dropped = atomic_load_explicit(&s_serial_tx.dropped,
                                                 memory_order_relaxed);
    stats_out->rx_depth = serial_queue_depth(&s_serial_rx);
    stats_out->rx_max_depth = atomic_load_explicit(&s_serial_rx.max_depth,
                                                   memory_order_relaxed);
    stats_out->rx_dropped = atomic_load_explicit(&s_serial_rx.dropped,
                                                 memory_order_relaxed);
}

static bool core_io_step_serial(RuntimeCoreIO *core)
{
    uint8_t byte;
    bool tx_empty = true;

    if (!core || !core->machine || !core->machine->uart_host.enabled)
        return true;

    /* Paula has strict priority over console logs. Peek first so FIFO-full
     * leaves the byte queued for the next Core 3 pass. */
    while (serial_queue_peek(&s_serial_tx, &byte)) {
        if (!uart_host_send_byte(&core->machine->uart_host, byte)) {
            tx_empty = false;
            break;
        }
        serial_queue_consume(&s_serial_tx);
    }

    while (uart_host_receive_byte(&core->machine->uart_host, &byte))
        (void)serial_queue_push(&s_serial_rx, byte);

    return tx_empty && serial_queue_depth(&s_serial_tx) == 0u;
}

bool core_io_init(RuntimeCoreIO *core, BellatrixMachine *machine)
{
    if (!core || !machine) {
        return false;
    }

    memset(core, 0, sizeof(*core));

    core->machine = machine;
    core->running = true;
    core->local_cycles = 0;
    memset(&s_serial_tx, 0, sizeof(s_serial_tx));
    memset(&s_serial_rx, 0, sizeof(s_serial_rx));
    usb_host_init(&core->usb_host);

    CORE3_LOG("init");
    return true;
}

void core_io_shutdown(RuntimeCoreIO *core)
{
    if (!core) {
        return;
    }

    extern BellatrixRuntime g_runtime;
    bt_host_shutdown(&g_runtime.bluetooth);
    usb_host_shutdown(&core->usb_host);

    CORE3_LOG("shutdown cycles=%llu", (unsigned long long)core->local_cycles);
    core->running = false;
}

bool core_io_open_debug_serial(RuntimeCoreIO *core)
{
    (void)core;
    return false;
}

/* Called by Core 3 via bellatrix_runtime_io_step() in pal_core.c. */
void core_io_step(RuntimeCoreIO *core, uint32_t cycles)
{
    if (!core || !core->running) {
        return;
    }

    core->local_cycles += cycles;

    extern BellatrixRuntime g_runtime;
    bt_host_step(&g_runtime.bluetooth);
    usb_host_step(&core->usb_host);

    /* Physical mini-UART belongs to Core 3 at runtime. Service Paula first;
     * logs are lowest priority and only drain when no Paula byte is waiting. */
    if (core_io_step_serial(core))
        console_log_drain();
}

/* Strong definition — overrides the weak stub in pal_core.c. Called from
 * Core 3's loop (multicore) or the PAL_Runtime_Poll throttle (single-core). */
void bellatrix_runtime_io_step(uint64_t now, uint64_t freq)
{
    (void)now;
    (void)freq;

    extern BellatrixRuntime g_runtime;
    core_io_step(&g_runtime.io, 1u);
}
