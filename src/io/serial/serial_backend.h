#ifndef BELLATRIX_IO_SERIAL_SERIAL_BACKEND_H
#define BELLATRIX_IO_SERIAL_SERIAL_BACKEND_H

#include <stdint.h>
#include <stdbool.h>

typedef struct SerialBackend SerialBackend;

typedef bool (*serial_backend_open_fn)(
    SerialBackend *backend);

typedef void (*serial_backend_close_fn)(
    SerialBackend *backend);

typedef bool (*serial_backend_read_fn)(
    SerialBackend *backend,
    uint8_t *byte_out);

typedef bool (*serial_backend_write_fn)(
    SerialBackend *backend,
    uint8_t byte);

typedef bool (*serial_backend_is_open_fn)(
    const SerialBackend *backend);

struct SerialBackend {
    void *userdata;

    serial_backend_open_fn open;
    serial_backend_close_fn close;

    serial_backend_read_fn read;
    serial_backend_write_fn write;

    serial_backend_is_open_fn is_open;
};

void serial_backend_init(SerialBackend *backend);

bool serial_backend_open(
    SerialBackend *backend);

void serial_backend_close(
    SerialBackend *backend);

bool serial_backend_read(
    SerialBackend *backend,
    uint8_t *byte_out);

bool serial_backend_write(
    SerialBackend *backend,
    uint8_t byte);

bool serial_backend_is_open(
    const SerialBackend *backend);

#endif