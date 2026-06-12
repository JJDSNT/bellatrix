#include "hal_time_ms.h"
#include "hal_tick.h"
#include "hal_cpu.h"
#include "hal_uart_dma.h"

#include "host/raspi3/time.h"
#include "host/raspi3/pl011_backend.h"
#include "support.h"

#include <stddef.h>
#include <stdarg.h>

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
static uint32_t bt_uart_io_activity = 0;

uint32_t bt_hal_raspi3_io_activity(void) {
    return bt_uart_io_activity;
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
        pl011_backend_close(&bt_uart);
    }
    return pl011_backend_open_flow(&bt_uart, baud, true) ? 0 : -1;
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

void bt_hal_raspi3_poll_uart(void) {
    if (!pl011_backend_is_open(&bt_uart)) {
        return;
    }

    if (uart_rx_buffer && uart_rx_pos < uart_rx_size) {
        uint8_t byte;
        while (pl011_backend_read_byte(&bt_uart, &byte)) {
            bt_uart_io_activity++;
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
            bt_uart_io_activity++;
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

    (void)uart_cts_irq_cb;
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
