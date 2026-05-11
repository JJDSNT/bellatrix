#include "io/serial/null_modem.h"

#include <string.h>

static const uint8_t s_diagrom_serial_test[] =
    "This is a serialporttest for loopbackadapter! NOT Console!\n\r";

static bool null_modem_loopback_push(NullModem *nm, uint8_t byte)
{
    if (!nm || nm->loopback_count >= (uint16_t)sizeof(nm->loopback_fifo)) {
        return false;
    }

    nm->loopback_fifo[nm->loopback_wr] = byte;
    nm->loopback_wr = (uint16_t)((nm->loopback_wr + 1u) %
                                 (uint16_t)sizeof(nm->loopback_fifo));
    nm->loopback_count++;
    return true;
}

static bool null_modem_loopback_pop(NullModem *nm, uint8_t *byte_out)
{
    if (!nm || !byte_out || nm->loopback_count == 0) {
        return false;
    }

    *byte_out = nm->loopback_fifo[nm->loopback_rd];
    nm->loopback_rd = (uint16_t)((nm->loopback_rd + 1u) %
                                 (uint16_t)sizeof(nm->loopback_fifo));
    nm->loopback_count--;
    return true;
}

static void null_modem_diagrom_reset(NullModem *nm)
{
    if (!nm) {
        return;
    }

    nm->diagrom_stream_index = 0;
}

static bool null_modem_diagrom_should_echo(NullModem *nm, uint8_t byte)
{
    size_t pattern_len;

    if (!nm) {
        return false;
    }

    if (byte == (uint8_t)'<' || byte == (uint8_t)'>') {
        null_modem_diagrom_reset(nm);
        return true;
    }

    pattern_len = sizeof(s_diagrom_serial_test) - 1u;

    if (nm->diagrom_stream_index < pattern_len &&
        byte == s_diagrom_serial_test[nm->diagrom_stream_index]) {
        nm->diagrom_stream_index++;
        if (nm->diagrom_stream_index >= pattern_len) {
            nm->diagrom_stream_index = 0;
        }
        return true;
    }

    nm->diagrom_stream_index = 0;

    if (byte == s_diagrom_serial_test[0]) {
        nm->diagrom_stream_index = 1;
        return true;
    }

    return false;
}

void null_modem_init(NullModem *nm,
                     void *opaque,
                     null_modem_send_cb send_cb,
                     null_modem_recv_cb recv_cb)
{
    if (!nm) {
        return;
    }

    memset(nm, 0, sizeof(*nm));

    nm->opaque = opaque;
    nm->send_cb = send_cb;
    nm->recv_cb = recv_cb;
    nm->mode = NULL_MODEM_CROSSED;
}

void null_modem_set_mode(NullModem *nm, NullModemMode mode)
{
    if (!nm) {
        return;
    }

    nm->mode = mode;
    nm->loopback_budget = 0;
    null_modem_diagrom_reset(nm);
}

NullModemMode null_modem_get_mode(const NullModem *nm)
{
    if (!nm) {
        return NULL_MODEM_CROSSED;
    }

    return nm->mode;
}

bool null_modem_send_from_amiga(NullModem *nm, uint8_t byte)
{
    if (!nm || !nm->send_cb) {
        return false;
    }

    switch (nm->mode) {
    case NULL_MODEM_CROSSED:
        return nm->send_cb(nm->opaque, byte);

    case NULL_MODEM_STRAIGHT_THROUGH:
        return nm->send_cb(nm->opaque, byte);

    case NULL_MODEM_LOOPBACK:
        /*
         * Loopback interno real para que software como DiagROM detecte
         * imediatamente o eco do próprio TX no RX. Ainda espelhamos o byte
         * para fora, para manter o console serial visível no host.
         */
        null_modem_loopback_push(nm, byte);
        return nm->send_cb(nm->opaque, byte);

    case NULL_MODEM_LOOPBACK_ONESHOT:
        if (null_modem_diagrom_should_echo(nm, byte)) {
            null_modem_loopback_push(nm, byte);
        }
        return nm->send_cb(nm->opaque, byte);

    default:
        return false;
    }
}

bool null_modem_receive_to_amiga(NullModem *nm, uint8_t *byte_out)
{
    if (!nm || !byte_out || !nm->recv_cb) {
        return false;
    }

    switch (nm->mode) {
    case NULL_MODEM_CROSSED:
        return nm->recv_cb(nm->opaque, byte_out);

    case NULL_MODEM_STRAIGHT_THROUGH:
        return nm->recv_cb(nm->opaque, byte_out);

    case NULL_MODEM_LOOPBACK:
        if (null_modem_loopback_pop(nm, byte_out)) {
            return true;
        }
        return nm->recv_cb(nm->opaque, byte_out);

    case NULL_MODEM_LOOPBACK_ONESHOT:
        if (null_modem_loopback_pop(nm, byte_out)) {
            return true;
        }
        return nm->recv_cb(nm->opaque, byte_out);

    default:
        return false;
    }
}
