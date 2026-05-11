#ifndef BELLATRIX_IO_SERIAL_TCP_SERIAL_H
#define BELLATRIX_IO_SERIAL_TCP_SERIAL_H

#include <stdint.h>
#include <stdbool.h>

typedef struct TCPSerial {
    int listen_fd;
    int client_fd;

    uint16_t port;

    bool server_open;
    bool client_connected;
} TCPSerial;

bool tcp_serial_open(TCPSerial *tcp,
                     uint16_t port);

void tcp_serial_close(TCPSerial *tcp);

bool tcp_serial_is_open(
    const TCPSerial *tcp);

bool tcp_serial_client_connected(
    const TCPSerial *tcp);

bool tcp_serial_read_byte(
    TCPSerial *tcp,
    uint8_t *byte_out);

bool tcp_serial_write_byte(
    TCPSerial *tcp,
    uint8_t byte);

#endif