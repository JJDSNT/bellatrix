#ifndef BELLATRIX_IO_SERIAL_NULL_MODEM_H
#define BELLATRIX_IO_SERIAL_NULL_MODEM_H

#include <stdint.h>
#include <stdbool.h>

typedef enum NullModemMode {
    NULL_MODEM_CROSSED = 0,
    NULL_MODEM_STRAIGHT_THROUGH = 1,
    NULL_MODEM_LOOPBACK = 2,
    NULL_MODEM_LOOPBACK_ONESHOT = 3
} NullModemMode;

typedef bool (*null_modem_send_cb)(void *opaque, uint8_t byte);
typedef bool (*null_modem_recv_cb)(void *opaque, uint8_t *byte_out);

typedef struct NullModem {
    void *opaque;

    null_modem_send_cb send_cb;
    null_modem_recv_cb recv_cb;

    NullModemMode mode;

    uint8_t loopback_fifo[256];
    uint16_t loopback_rd;
    uint16_t loopback_wr;
    uint16_t loopback_count;
    uint16_t loopback_budget;
} NullModem;

void null_modem_init(NullModem *nm,
                     void *opaque,
                     null_modem_send_cb send_cb,
                     null_modem_recv_cb recv_cb);

void null_modem_set_mode(NullModem *nm, NullModemMode mode);
NullModemMode null_modem_get_mode(const NullModem *nm);

bool null_modem_send_from_amiga(NullModem *nm, uint8_t byte);
bool null_modem_receive_to_amiga(NullModem *nm, uint8_t *byte_out);

#endif
