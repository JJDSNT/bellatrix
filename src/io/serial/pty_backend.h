#ifndef BELLATRIX_IO_SERIAL_PTY_BACKEND_H
#define BELLATRIX_IO_SERIAL_PTY_BACKEND_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct PtyBackend {
    int master_fd;
    char slave_name[128];
    bool open;
} PtyBackend;

bool pty_backend_open(PtyBackend *pty);
void pty_backend_close(PtyBackend *pty);

bool pty_backend_is_open(const PtyBackend *pty);
const char *pty_backend_slave_name(const PtyBackend *pty);

bool pty_backend_read_byte(PtyBackend *pty, uint8_t *byte_out);
bool pty_backend_write_byte(PtyBackend *pty, uint8_t byte);

#endif