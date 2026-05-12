#include "hal_time_ms.h"
#include "hal_tick.h"
#include "hal_cpu.h"
#include "btstack_uart_block.h"
#include "btstack_run_loop_embedded.h"

#include "host/raspi3/time.h"
#include "host/raspi3/pl011_backend.h"
#include "debug/core_log.h"
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
    asm volatile("wfe");
}

// --- HAL UART BLOCK ---

static void (*uart_block_received_cb)(void);
static void (*uart_block_sent_cb)(void);

static uint8_t *uart_rx_buffer;
static uint16_t uart_rx_size;
static uint16_t uart_rx_pos;

static const uint8_t *uart_tx_buffer;
static uint16_t uart_tx_size;
static uint16_t uart_tx_pos;

static PL011Backend bt_uart;

static int bt_uart_init(const btstack_uart_config_t * config) {
    if (!pl011_backend_open(&bt_uart, config->baudrate)) {
        kprintf("[BT-HAL] Failed to open PL011\n");
        return -1;
    }
    return 0;
}

static void bt_uart_set_block_received(void (*handler)(void)) {
    uart_block_received_cb = handler;
}

static void bt_uart_set_block_sent(void (*handler)(void)) {
    uart_block_sent_cb = handler;
}

static void bt_uart_receive_block(uint8_t *buffer, uint16_t len) {
    uart_rx_buffer = buffer;
    uart_rx_size = len;
    uart_rx_pos = 0;
}

static void bt_uart_send_block(const uint8_t *buffer, uint16_t len) {
    uart_tx_buffer = buffer;
    uart_tx_size = len;
    uart_tx_pos = 0;
}

static int bt_uart_set_baudrate(uint32_t baudrate) {
    return pl011_backend_open(&bt_uart, baudrate) ? 0 : -1;
}

void bt_hal_raspi3_poll_uart(void) {
    if (uart_rx_buffer && uart_rx_pos < uart_rx_size) {
        uint8_t byte;
        while (pl011_backend_read_byte(&bt_uart, &byte)) {
            uart_rx_buffer[uart_rx_pos++] = byte;
            if (uart_rx_pos == uart_rx_size) {
                void (*cb)(void) = uart_block_received_cb;
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
            uart_tx_pos++;
        }
        if (uart_tx_pos == uart_tx_size) {
            void (*cb)(void) = uart_block_sent_cb;
            uart_tx_buffer = NULL;
            if (cb) cb();
        }
    }
}

static const btstack_uart_block_t bt_uart_driver = {
    .init = bt_uart_init,
    .set_block_received = bt_uart_set_block_received,
    .set_block_sent = bt_uart_set_block_sent,
    .receive_block = bt_uart_receive_block,
    .send_block = bt_uart_send_block,
    .set_baudrate = bt_uart_set_baudrate,
};

const btstack_uart_block_t * btstack_uart_block_embedded_instance(void) {
    return &bt_uart_driver;
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
