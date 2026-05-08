#include "chipset/paula/paula_serial.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct TestSerialSink {
    uint16_t last_irq_mask;
    int irq_count;
} TestSerialSink;

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

static void test_irq_cb(void *opaque, uint16_t mask)
{
    TestSerialSink *sink = (TestSerialSink *)opaque;

    sink->last_irq_mask = mask;
    sink->irq_count += 1;
}

static void test_serdat_queues_one_tx_byte(void)
{
    PaulaSerial serial;
    TestSerialSink sink = {0};
    uint16_t serdatr = 0;
    uint8_t tx_byte = 0;

    paula_serial_init(&serial, &sink, test_irq_cb);

    paula_serial_write_serdat(&serial, 0x0141u);

    CHECK_EQ("SERDAT raises TBE", PAULA_SERIAL_INTREQ_TBE, sink.last_irq_mask);
    CHECK_EQ("default Paula TX is not instant", 0u, paula_serial_tx_available(&serial) ? 1u : 0u);
    serdatr = paula_serial_read_serdatr(&serial);
    CHECK_EQ("SERDATR reports TBE after load into shift register", 0x2000u, serdatr & 0x2000u);
    CHECK_EQ("SERDATR reports TSRE low while shift is active", 0x0000u, serdatr & 0x1000u);
    paula_serial_step(&serial, 10u);
    CHECK_EQ("one TX byte is queued after one frame", 1u, paula_serial_tx_available(&serial) ? 1u : 0u);
    CHECK_EQ("peek queued TX byte", 1u, paula_serial_peek_tx_byte(&serial, &tx_byte) ? 1u : 0u);
    CHECK_EQ("SERDAT payload", 0x41u, tx_byte);
    CHECK_EQ("pop queued TX byte", 1u, paula_serial_pop_tx_byte(&serial, &tx_byte) ? 1u : 0u);
    CHECK_EQ("popped payload", 0x41u, tx_byte);
    CHECK_EQ("queue empty after pop", 0u, paula_serial_tx_available(&serial) ? 1u : 0u);
}

static void test_serdat_instant_mode_still_available(void)
{
    PaulaSerial serial;
    TestSerialSink sink = {0};
    uint8_t tx_byte = 0;

    paula_serial_init(&serial, &sink, test_irq_cb);
    paula_serial_set_tx_instant(&serial, true);
    paula_serial_write_serdat(&serial, 0x0142u);

    CHECK_EQ("instant mode queues tx immediately", 1u, paula_serial_tx_available(&serial) ? 1u : 0u);
    CHECK_EQ("instant mode payload visible", 1u, paula_serial_peek_tx_byte(&serial, &tx_byte) ? 1u : 0u);
    CHECK_EQ("instant mode payload value", 0x42u, tx_byte);
}

static void test_receive_byte_appears_in_serdatr(void)
{
    PaulaSerial serial;
    TestSerialSink sink = {0};
    uint16_t serdatr;

    paula_serial_init(&serial, &sink, test_irq_cb);

    paula_serial_receive_byte(&serial, 0x55u);

    CHECK_EQ("backend TX raises RBF", PAULA_SERIAL_INTREQ_RBF, sink.last_irq_mask);
    serdatr = paula_serial_read_serdatr(&serial);
    CHECK_EQ("SERDATR byte visible", 0x55u, serdatr & 0x00ffu);
    CHECK_EQ("SERDATR marks receive full", 0x4000u, serdatr & 0x4000u);

    paula_serial_clear_rbf(&serial);
    serdatr = paula_serial_read_serdatr(&serial);
    CHECK_EQ("SERDATR empty low data bits after read", 0x0000u, serdatr & 0x03ffu);
}

int main(void)
{
    test_serdat_queues_one_tx_byte();
    test_serdat_instant_mode_still_available();
    test_receive_byte_appears_in_serdatr();

    puts("bellatrix_unit_paula_serial: ok");
    return 0;
}
