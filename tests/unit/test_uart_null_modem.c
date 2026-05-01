#include "chipset/paula/uart.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct TestUartSink {
    uint8_t last_tx_byte;
    uint16_t last_irq_mask;
    int tx_count;
    int irq_count;
} TestUartSink;

static void failf(const char *expr, const char *file, int line,
                  uint32_t expected, uint32_t actual)
{
    fprintf(stderr,
            "FAIL %s:%d %s expected=0x%08x actual=0x%08x\n",
            file, line, expr, expected, actual);
    exit(1);
}

#define CHECK_EQ(expr, expected, actual)                                      \
    do                                                                        \
    {                                                                         \
        uint32_t expected__ = (uint32_t)(expected);                           \
        uint32_t actual__ = (uint32_t)(actual);                               \
        if (expected__ != actual__)                                           \
            failf((expr), __FILE__, __LINE__, expected__, actual__);          \
    } while (0)

static void test_tx_cb(void *opaque, uint8_t byte)
{
    TestUartSink *sink = (TestUartSink *)opaque;

    sink->last_tx_byte = byte;
    sink->tx_count += 1;
}

static void test_irq_cb(void *opaque, uint16_t mask)
{
    TestUartSink *sink = (TestUartSink *)opaque;

    sink->last_irq_mask = mask;
    sink->irq_count += 1;
}

static void test_default_mode_is_null_modem(void)
{
    UARTState uart;
    TestUartSink sink = {0};

    uart_init(&uart, &sink, test_tx_cb, test_irq_cb);

    CHECK_EQ("default link mode", UART_LINK_NULL_MODEM, uart_link_mode(&uart));

    uart_write_serdat(&uart, 0x0141u);

    CHECK_EQ("SERDAT sends one byte to partner RX", 1u, sink.tx_count);
    CHECK_EQ("SERDAT payload", 0x41u, sink.last_tx_byte);
    CHECK_EQ("SERDAT raises TBE", UART_INTREQ_TBE, sink.last_irq_mask);
}

static void test_receive_byte_appears_in_serdatr(void)
{
    UARTState uart;
    TestUartSink sink = {0};
    uint16_t serdatr;

    uart_init(&uart, &sink, test_tx_cb, test_irq_cb);

    uart_receive_byte(&uart, 0x55u);

    CHECK_EQ("backend TX raises RBF", UART_INTREQ_RBF, sink.last_irq_mask);
    serdatr = uart_read_serdatr(&uart);
    CHECK_EQ("SERDATR byte visible", 0x55u, serdatr & 0x00ffu);
    CHECK_EQ("SERDATR marks receive full", 0x4000u, serdatr & 0x4000u);

    uart_clear_rbf(&uart);
    serdatr = uart_read_serdatr(&uart);
    CHECK_EQ("SERDATR empty low data bits after read", 0x0000u, serdatr & 0x03ffu);
}

int main(void)
{
    test_default_mode_is_null_modem();
    test_receive_byte_appears_in_serdatr();

    puts("bellatrix_unit_uart: ok");
    return 0;
}
