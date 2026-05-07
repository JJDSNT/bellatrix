#include "io/serial/serial_backend.h"

#include <string.h>

void serial_backend_init(SerialBackend *backend)
{
    if (!backend) {
        return;
    }

    memset(backend, 0, sizeof(*backend));
}

bool serial_backend_open(
    SerialBackend *backend)
{
    if (!backend || !backend->open) {
        return false;
    }

    return backend->open(backend);
}

void serial_backend_close(
    SerialBackend *backend)
{
    if (!backend || !backend->close) {
        return;
    }

    backend->close(backend);
}

bool serial_backend_read(
    SerialBackend *backend,
    uint8_t *byte_out)
{
    if (!backend ||
        !backend->read ||
        !byte_out)
    {
        return false;
    }

    return backend->read(
        backend,
        byte_out);
}

bool serial_backend_write(
    SerialBackend *backend,
    uint8_t byte)
{
    if (!backend ||
        !backend->write)
    {
        return false;
    }

    return backend->write(
        backend,
        byte);
}

bool serial_backend_is_open(
    const SerialBackend *backend)
{
    if (!backend ||
        !backend->is_open)
    {
        return false;
    }

    return backend->is_open(
        backend);
}