#include "hal_time_ms.h"
#include "hal_tick.h"
#include "hal_cpu.h"
#include "hal_uart_dma.h"

#include "host/raspi3/time.h"
#include "host/raspi3/pl011_backend.h"
#include "host/raspi3/physical_interrupts.h"
#include "io/bluetooth/bt_diag.h"
#include "io/bluetooth/bt_hal_raspi3.h"
#include "runtime/core_io.h"
#include "support.h"

#include <stddef.h>
#include <stdarg.h>
#include <stdatomic.h>

// --- HAL TIME / TICK ---

static void (*tick_handler)(void) = NULL;

uint32_t hal_time_ms(void) {
    extern uint64_t raspi3_read_legacy_system_timer(void);
    return (uint32_t)(raspi3_read_legacy_system_timer() / 1000u);
}

void hal_tick_init(void) {
}

void hal_tick_set_handler(void (*handler)(void)) {
    tick_handler = handler;
}

int hal_tick_get_tick_period_in_ms(void) {
    return 1;
}

// --- HAL CPU ---

void hal_cpu_disable_irqs(void) {
    asm volatile("msr daifset, #2" ::: "memory");
}

void hal_cpu_enable_irqs(void) {
    asm volatile("msr daifclr, #2" ::: "memory");
}

void hal_cpu_enable_irqs_and_sleep(void) {
    hal_cpu_enable_irqs();
    /* NO wfe here.  Bellatrix drives the btstack run loop by polling
     * (bt_host_step → execute_once); on single-core, sev-free bare metal
     * with no routed IRQs a real WFE parks the core forever the moment no
     * event happens to be pending — observed as a silent boot hang right
     * after "waiting for bootstrap window".  Earlier builds only survived
     * because stray pending interrupts (USB) kept waking the WFE. */
    asm volatile("yield");
}

// --- HAL UART DMA over PL011 ---

static void (*uart_block_received_cb)(void) = NULL;
static void (*uart_block_sent_cb)(void) = NULL;
static void (*uart_cts_irq_cb)(void) = NULL;

static uint8_t *uart_rx_buffer = NULL;
static uint16_t uart_rx_size = 0;
static uint16_t uart_rx_pos = 0;

static const uint8_t *uart_tx_buffer = NULL;
static uint16_t uart_tx_size = 0;
static uint16_t uart_tx_pos = 0;

static PL011Backend bt_uart;
static uint32_t bt_uart_baudrate = 115200;
static uint32_t bt_uart_tx_blocks = 0;
static uint32_t bt_uart_rx_blocks = 0;
static uint32_t bt_uart_rx_bytes_logged = 0;
static uint32_t bt_uart_tx_bytes_logged = 0;
/* uncapped rx+tx byte counter — lets bootstrap timeouts distinguish a dead
 * link from a slow PatchRAM upload that is still making progress */
static _Atomic uint32_t bt_uart_io_activity;
static _Atomic uint32_t bt_uart_tx_total;
static _Atomic uint32_t bt_uart_rx_total;

/* Software RX ring buffer — decouples hardware FIFO drain from the btstack
 * H4 parser.  bt_hal_raspi3_drain_fifo() can be called from anywhere (e.g.
 * framebuffer fill loops) without touching btstack state; the poll path
 * consumes bytes from the ring at its normal cadence.
 *
 * Future improvement: replace the drain call sites with a FIQ handler that
 * routes UART0 IRQ (BCM2835 source 57) via the FIQ control register
 * (ARM_PERI_VIRT_BASE+0xB20C, value 0x80|57=0xB9) and drains the FIFO in
 * the curr_el_spx_fiq vector (currently a redundant copy of the IRQ handler).
 * That would eliminate all pump-gap overruns without polling overhead.
 * Requires a patch to emu68/src/aarch64/vectors.c — deferred pending
 * architecture review. */
#define BT_RX_RING_SIZE 4096u   /* power of 2; fits ~35 ms of max-rate LE adverts */
static uint8_t  s_rx_ring[BT_RX_RING_SIZE];
static _Atomic uint32_t s_rx_ring_head; /* producer: Core 0 IRQ/poll */
static _Atomic uint32_t s_rx_ring_tail; /* consumer: Core 3 reactor */
static _Atomic uint32_t s_rx_ring_overflow;
static _Atomic uint32_t s_irq_rx_bytes;
static _Atomic uint32_t s_irq_rx_budget_hits;
/* wire tap: dump the next N raw rx/tx bytes into the bt_diag ring so the SD
 * report shows what is actually on the line (armed by bt_scan_start) */
static uint32_t bt_diag_tap_rx_remaining = 0;
static uint32_t bt_diag_tap_tx_remaining = 0;

uint32_t bt_hal_raspi3_io_activity(void) {
    return atomic_load_explicit(&bt_uart_io_activity, memory_order_relaxed);
}

uint32_t bt_hal_raspi3_io_tx(void) {
    return atomic_load_explicit(&bt_uart_tx_total, memory_order_relaxed);
}

uint32_t bt_hal_raspi3_io_rx(void) {
    return atomic_load_explicit(&bt_uart_rx_total, memory_order_relaxed);
}

void bt_hal_raspi3_diag_tap(uint32_t rx_bytes, uint32_t tx_bytes) {
    bt_diag_tap_rx_remaining = rx_bytes;
    bt_diag_tap_tx_remaining = tx_bytes;
}

/* H4 parser state: what the transport is currently waiting for.  A stall
 * "waiting 200+ bytes, few filled" = parser desync after a lost byte; a
 * stall "waiting 1 byte, 0 filled" with rx frozen = controller went mute.
 * filled counts bytes already in the H4 block (consumed from ring); the
 * ring backlog is a separate diagnostic available via bt_hal_raspi3_rx_ring_used(). */
void bt_hal_raspi3_rx_pending(uint32_t *filled, uint32_t *wanted) {
    if (filled) *filled = (uint32_t)uart_rx_pos;
    if (wanted) *wanted = (uint32_t)uart_rx_size;
}

uint32_t bt_hal_raspi3_rx_ring_used(void) {
    uint32_t head = atomic_load_explicit(&s_rx_ring_head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&s_rx_ring_tail, memory_order_acquire);
    return (head - tail) & (BT_RX_RING_SIZE - 1u);
}

uint32_t bt_hal_raspi3_irq_rx_bytes(void) {
    return atomic_load_explicit(&s_irq_rx_bytes, memory_order_relaxed);
}

uint32_t bt_hal_raspi3_irq_rx_budget_hits(void) {
    return atomic_load_explicit(&s_irq_rx_budget_hits, memory_order_relaxed);
}

uint32_t bt_hal_raspi3_rx_overflow(void) {
    return atomic_load_explicit(&s_rx_ring_overflow, memory_order_relaxed);
}

static bool bt_rx_ring_push(uint8_t byte)
{
    uint32_t head = atomic_load_explicit(&s_rx_ring_head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&s_rx_ring_tail, memory_order_acquire);
    uint32_t next = (head + 1u) & (BT_RX_RING_SIZE - 1u);
    if (next == tail) {
        atomic_fetch_add_explicit(&s_rx_ring_overflow, 1u,
                                  memory_order_relaxed);
        return false;
    }
    s_rx_ring[head] = byte;
    atomic_store_explicit(&s_rx_ring_head, next, memory_order_release);
    return true;
}

#define BT_UART_LOG_BYTES_MAX 32u
#define BT_UART_TRACE_MAX_EVENTS 256u

typedef enum {
    BT_TRACE_OPEN = 1,
    BT_TRACE_OPEN_FAILED,
    BT_TRACE_SET_BAUD,
    BT_TRACE_RX_REQUEST,
    BT_TRACE_TX_BLOCK,
    BT_TRACE_TX_BYTE,
    BT_TRACE_TX_COMPLETE,
    BT_TRACE_RX_BYTE,
    BT_TRACE_RX_COMPLETE
} BTTraceEventType;

typedef struct {
    uint8_t type;
    uint8_t arg0;
    uint32_t arg1;
    uint32_t arg2;
} BTTraceEvent;

static BTTraceEvent bt_uart_trace[BT_UART_TRACE_MAX_EVENTS];
static uint16_t bt_uart_trace_count = 0;
static uint16_t bt_uart_trace_dropped = 0;
static bool bt_uart_trace_wrapped = false;

static void bt_uart_trace_add(uint8_t type, uint8_t arg0, uint32_t arg1, uint32_t arg2)
{
    uint16_t index;

    if (bt_uart_trace_count < BT_UART_TRACE_MAX_EVENTS) {
        index = bt_uart_trace_count++;
    } else {
        index = BT_UART_TRACE_MAX_EVENTS - 1u;
        bt_uart_trace_dropped++;
        bt_uart_trace_wrapped = true;
    }

    bt_uart_trace[index].type = type;
    bt_uart_trace[index].arg0 = arg0;
    bt_uart_trace[index].arg1 = arg1;
    bt_uart_trace[index].arg2 = arg2;
}

void bt_hal_raspi3_trace_reset(void)
{
    bt_uart_trace_count = 0;
    bt_uart_trace_dropped = 0;
    bt_uart_trace_wrapped = false;
}

void bt_hal_raspi3_trace_dump(void)
{
    uint16_t i;

    kprintf("[BT-HAL] trace dump: events=%u dropped=%u%s\n",
            (unsigned)bt_uart_trace_count,
            (unsigned)bt_uart_trace_dropped,
            bt_uart_trace_wrapped ? " (truncated)" : "");

    for (i = 0; i < bt_uart_trace_count; ++i) {
        const BTTraceEvent *ev = &bt_uart_trace[i];

        switch ((BTTraceEventType)ev->type) {
            case BT_TRACE_OPEN:
                kprintf("[BT-HAL] trace[%u] open baud=%u\n",
                        (unsigned)i, (unsigned)ev->arg2);
                break;
            case BT_TRACE_OPEN_FAILED:
                kprintf("[BT-HAL] trace[%u] open failed baud=%u\n",
                        (unsigned)i, (unsigned)ev->arg2);
                break;
            case BT_TRACE_SET_BAUD:
                kprintf("[BT-HAL] trace[%u] set baud %u -> %u\n",
                        (unsigned)i, (unsigned)ev->arg2, (unsigned)ev->arg1);
                break;
            case BT_TRACE_RX_REQUEST:
                kprintf("[BT-HAL] trace[%u] rx request #%u len=%u\n",
                        (unsigned)i, (unsigned)ev->arg2, (unsigned)ev->arg1);
                break;
            case BT_TRACE_TX_BLOCK:
                kprintf("[BT-HAL] trace[%u] tx block len=%u first=%02x\n",
                        (unsigned)i, (unsigned)ev->arg1, (unsigned)ev->arg0);
                break;
            case BT_TRACE_TX_BYTE:
                kprintf("[BT-HAL] trace[%u] tx byte[%u]=%02x\n",
                        (unsigned)i, (unsigned)ev->arg2, (unsigned)ev->arg0);
                break;
            case BT_TRACE_TX_COMPLETE:
                kprintf("[BT-HAL] trace[%u] tx complete len=%u\n",
                        (unsigned)i, (unsigned)ev->arg1);
                break;
            case BT_TRACE_RX_BYTE:
                kprintf("[BT-HAL] trace[%u] rx byte[%u]=%02x\n",
                        (unsigned)i, (unsigned)ev->arg2, (unsigned)ev->arg0);
                break;
            case BT_TRACE_RX_COMPLETE:
                kprintf("[BT-HAL] trace[%u] rx complete len=%u\n",
                        (unsigned)i, (unsigned)ev->arg1);
                break;
            default:
                kprintf("[BT-HAL] trace[%u] unknown type=%u\n",
                        (unsigned)i, (unsigned)ev->type);
                break;
        }
    }
}

static void bt_uart_log_preview(const char *tag, const uint8_t *buffer, uint16_t length)
{
    uint16_t preview;

    if (!buffer || length == 0) {
        kprintf("[BT-HAL] %s len=0\n", tag);
        return;
    }

    preview = length < 8 ? length : 8;
    kprintf("[BT-HAL] %s len=%u data=%02x %02x %02x %02x %02x %02x %02x %02x%s\n",
            tag,
            (unsigned)length,
            preview > 0 ? buffer[0] : 0,
            preview > 1 ? buffer[1] : 0,
            preview > 2 ? buffer[2] : 0,
            preview > 3 ? buffer[3] : 0,
            preview > 4 ? buffer[4] : 0,
            preview > 5 ? buffer[5] : 0,
            preview > 6 ? buffer[6] : 0,
            preview > 7 ? buffer[7] : 0,
            length > preview ? " ..." : "");
}

void hal_uart_dma_init(void) {
    if (!pl011_backend_is_open(&bt_uart)) {
        if (!pl011_backend_open_flow(&bt_uart, bt_uart_baudrate, true)) {
            bt_uart_trace_add(BT_TRACE_OPEN_FAILED, 0, 0, bt_uart_baudrate);
            kprintf("[BT-HAL] Failed to open PL011 at %u baud\n", (unsigned)bt_uart_baudrate);
        } else {
            bt_uart_trace_add(BT_TRACE_OPEN, 0, 0, bt_uart_baudrate);
            kprintf("[BT-HAL] PL011 opened at %u baud\n", (unsigned)bt_uart_baudrate);
            pl011_backend_rx_irq_enable(true);
            bellatrix_physical_bt_irq_enable();
        }
    }
}

void hal_uart_dma_set_block_received(void (*callback)(void)) {
    uart_block_received_cb = callback;
}

void hal_uart_dma_set_block_sent(void (*callback)(void)) {
    uart_block_sent_cb = callback;
}

int hal_uart_dma_set_baud(uint32_t baud) {
    uint32_t old_baud = bt_uart_baudrate;

    bt_uart_trace_add(BT_TRACE_SET_BAUD, 0, baud, old_baud);
    kprintf("[BT-HAL] set baud %u -> %u\n",
            (unsigned)old_baud, (unsigned)baud);
    bt_uart_baudrate = baud;
    if (pl011_backend_is_open(&bt_uart)) {
        pl011_backend_rx_irq_enable(false);
        bellatrix_physical_bt_irq_disable();
        pl011_backend_close(&bt_uart);
    }
    if (!pl011_backend_open_flow(&bt_uart, baud, true))
        return -1;
    pl011_backend_rx_irq_enable(true);
    bellatrix_physical_bt_irq_enable();
    return 0;
}

void hal_uart_dma_send_block(const uint8_t *buffer, uint16_t length) {
    bt_uart_tx_blocks++;
    bt_uart_trace_add(BT_TRACE_TX_BLOCK, buffer && length ? buffer[0] : 0, length, bt_uart_tx_blocks);
    bt_uart_log_preview("tx block", buffer, length);
    uart_tx_buffer = buffer;
    uart_tx_size = length;
    uart_tx_pos = 0;
}

void hal_uart_dma_receive_block(uint8_t *buffer, uint16_t len) {
    bt_uart_rx_blocks++;
    bt_uart_trace_add(BT_TRACE_RX_REQUEST, 0, len, bt_uart_rx_blocks);
    kprintf("[BT-HAL] rx block request #%u len=%u\n",
            (unsigned)bt_uart_rx_blocks, (unsigned)len);
    uart_rx_buffer = buffer;
    uart_rx_size = len;
    uart_rx_pos = 0;
}

void hal_uart_dma_set_csr_irq_handler(void (*csr_irq_handler)(void)) {
    uart_cts_irq_cb = csr_irq_handler;
}

void hal_uart_dma_set_sleep(uint8_t sleep) {
    (void)sleep;
}

/* Drain the hardware RX FIFO into the software ring buffer.  Safe to call
 * from any context (framebuffer fill loops, USB pump, etc.) — never touches
 * btstack state or callbacks.  Bytes sit in the ring until poll_uart picks
 * them up and feeds them to the H4 parser. */
void bt_hal_raspi3_drain_fifo(void)
{
    uint8_t byte;
    uint32_t budget = 256u;
    if (!pl011_backend_is_open(&bt_uart) ||
        bellatrix_physical_bt_irq_is_armed())
        return;
    while (budget-- && pl011_backend_read_byte(&bt_uart, &byte)) {
        (void)bt_rx_ring_push(byte);
        atomic_fetch_add_explicit(&bt_uart_io_activity, 1u,
                                  memory_order_relaxed);
        atomic_fetch_add_explicit(&bt_uart_rx_total, 1u,
                                  memory_order_relaxed);
        if (bt_diag_tap_rx_remaining) {
            bt_diag_tap_rx_remaining--;
            bt_diag_log("[BT-RX] %02x\n", (unsigned)byte);
        }
    }
}

/* Link-recovery helper: throw away whatever half-parsed garbage is in
 * flight (stale FIFO bytes + ring contents + the H4 block stuck mid-fill)
 * so the stack can restart from a clean line after an RX overrun desync. */
void bt_hal_raspi3_flush_rx(void) {
    uint8_t byte;
    bellatrix_physical_bt_irq_disable();
    pl011_backend_rx_irq_enable(false);
    while (pl011_backend_is_open(&bt_uart) &&
           pl011_backend_read_byte(&bt_uart, &byte)) { }
    atomic_store_explicit(&s_rx_ring_head, 0u, memory_order_release);
    atomic_store_explicit(&s_rx_ring_tail, 0u, memory_order_release);
    uart_rx_buffer = NULL;
    uart_rx_size = 0;
    uart_rx_pos = 0;
    if (pl011_backend_is_open(&bt_uart)) {
        pl011_backend_rx_irq_enable(true);
        bellatrix_physical_bt_irq_enable();
    }
}

void bt_hal_raspi3_poll_uart(void) {
    if (!pl011_backend_is_open(&bt_uart)) {
        return;
    }

    /* Always drain hardware FIFO into the ring first so that bytes arriving
     * during any gap (framebuffer writes, USB pump, etc.) are not lost even
     * when the btstack H4 parser is not ready to consume them yet. */
    bt_hal_raspi3_drain_fifo();

    if (uart_rx_buffer && uart_rx_pos < uart_rx_size) {
        for (;;) {
            uint32_t tail = atomic_load_explicit(&s_rx_ring_tail,
                                                 memory_order_relaxed);
            uint32_t head = atomic_load_explicit(&s_rx_ring_head,
                                                 memory_order_acquire);
            if (tail == head)
                break;
            uint8_t byte = s_rx_ring[tail];
            atomic_store_explicit(&s_rx_ring_tail,
                                  (tail + 1u) & (BT_RX_RING_SIZE - 1u),
                                  memory_order_release);
            bt_uart_trace_add(BT_TRACE_RX_BYTE, byte, 0, bt_uart_rx_bytes_logged);
            if (bt_uart_rx_bytes_logged < BT_UART_LOG_BYTES_MAX) {
                kprintf("[BT-HAL] rx byte[%u]=%02x\n",
                        (unsigned)bt_uart_rx_bytes_logged, (unsigned)byte);
                bt_uart_rx_bytes_logged++;
            }
            uart_rx_buffer[uart_rx_pos++] = byte;
            if (uart_rx_pos == uart_rx_size) {
                void (*cb)(void) = uart_block_received_cb;
                bt_uart_trace_add(BT_TRACE_RX_COMPLETE, 0, uart_rx_size, 0);
                kprintf("[BT-HAL] rx block complete len=%u\n", (unsigned)uart_rx_size);
                uart_rx_buffer = NULL;
                if (cb) cb();
                break;
            }
        }
    }

    if (uart_tx_buffer && uart_tx_pos < uart_tx_size) {
        while (uart_tx_pos < uart_tx_size) {
            if (!pl011_backend_write_byte(&bt_uart, uart_tx_buffer[uart_tx_pos])) {
                break;
            }
            atomic_fetch_add_explicit(&bt_uart_io_activity, 1u,
                                      memory_order_relaxed);
            atomic_fetch_add_explicit(&bt_uart_tx_total, 1u,
                                      memory_order_relaxed);
            if (bt_diag_tap_tx_remaining) {
                bt_diag_tap_tx_remaining--;
                bt_diag_log("[BT-TX] %02x\n", (unsigned)uart_tx_buffer[uart_tx_pos]);
            }
            bt_uart_trace_add(BT_TRACE_TX_BYTE, uart_tx_buffer[uart_tx_pos], 0, bt_uart_tx_bytes_logged);
            if (bt_uart_tx_bytes_logged < BT_UART_LOG_BYTES_MAX) {
                kprintf("[BT-HAL] tx byte[%u]=%02x\n",
                        (unsigned)bt_uart_tx_bytes_logged,
                        (unsigned)uart_tx_buffer[uart_tx_pos]);
                bt_uart_tx_bytes_logged++;
            }
            uart_tx_pos++;
        }
        if (uart_tx_pos == uart_tx_size) {
            void (*cb)(void) = uart_block_sent_cb;
            bt_uart_trace_add(BT_TRACE_TX_COMPLETE, 0, uart_tx_size, 0);
            kprintf("[BT-HAL] tx block complete len=%u\n", (unsigned)uart_tx_size);
            uart_tx_buffer = NULL;
            if (cb) cb();
        }
    }

    /* RX/RT is one-shot. Only normal reactor context exposes it again. */
    pl011_backend_rx_irq_enable(true);
    bellatrix_physical_bt_irq_rearm();

    (void)uart_cts_irq_cb;
}

void __attribute__((target("general-regs-only"))) bt_hal_raspi3_irq_rx(void)
{
    uint8_t byte;
    uint32_t drained = 0u;
    const uint32_t budget = 64u;

    /* No BTstack, callback, allocation or logging is allowed here. */
    pl011_backend_rx_irq_enable(false);
    while (drained < budget && pl011_backend_is_open(&bt_uart) &&
           pl011_backend_read_byte(&bt_uart, &byte)) {
        (void)bt_rx_ring_push(byte);
        atomic_fetch_add_explicit(&bt_uart_io_activity, 1u,
                                  memory_order_relaxed);
        atomic_fetch_add_explicit(&bt_uart_rx_total, 1u,
                                  memory_order_relaxed);
        atomic_fetch_add_explicit(&s_irq_rx_bytes, 1u,
                                  memory_order_relaxed);
        drained++;
    }
    if (drained == budget)
        atomic_fetch_add_explicit(&s_irq_rx_budget_hits, 1u,
                                  memory_order_relaxed);
    core_io_notify(CORE_IO_EVENT_BLUETOOTH);
}

// --- REDIRECT PRINTF ---

typedef struct {
    char *buf;
    size_t size;
    size_t pos;
} vsnprintf_ctx_t;

static void vsnprintf_putc(void *data, char c) {
    vsnprintf_ctx_t *ctx = (vsnprintf_ctx_t *)data;
    if (ctx->size > 0 && ctx->pos + 1 < ctx->size) {
        ctx->buf[ctx->pos++] = c;
        ctx->buf[ctx->pos] = 0;
    }
}

int vsnprintf(char *str, size_t size, const char *format, va_list ap) {
    vsnprintf_ctx_t ctx = {str, size, 0};
    if (size > 0) str[0] = 0;
    vkprintf_pc(vsnprintf_putc, &ctx, format, ap);
    return (int)ctx.pos;
}

int printf(const char * format, ...) {
    va_list args;
    va_start(args, format);
    vkprintf(format, args);
    va_end(args);
    return 0;
}
