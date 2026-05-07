#define _GNU_SOURCE
#include "io/serial/pty_backend.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
#include <stdlib.h>
#include <termios.h>
#endif

static void pty_backend_reset(PtyBackend *pty)
{
    if (!pty) {
        return;
    }

    pty->master_fd = -1;
    pty->slave_name[0] = '\0';
    pty->open = false;
}

bool pty_backend_open(PtyBackend *pty)
{
    if (!pty) {
        return false;
    }

    pty_backend_reset(pty);

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
    int fd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return false;
    }

    if (grantpt(fd) != 0 || unlockpt(fd) != 0) {
        close(fd);
        return false;
    }

    char *name = ptsname(fd);
    if (!name) {
        close(fd);
        return false;
    }

    struct termios tio;
    if (tcgetattr(fd, &tio) == 0) {
        cfmakeraw(&tio);
        tcsetattr(fd, TCSANOW, &tio);
    }

    strncpy(pty->slave_name, name, sizeof(pty->slave_name) - 1);
    pty->slave_name[sizeof(pty->slave_name) - 1] = '\0';

    pty->master_fd = fd;
    pty->open = true;

    return true;
#else
    return false;
#endif
}

void pty_backend_close(PtyBackend *pty)
{
    if (!pty || !pty->open) {
        return;
    }

    close(pty->master_fd);
    pty_backend_reset(pty);
}

bool pty_backend_is_open(const PtyBackend *pty)
{
    return pty && pty->open;
}

const char *pty_backend_slave_name(const PtyBackend *pty)
{
    if (!pty || !pty->open) {
        return NULL;
    }

    return pty->slave_name;
}

bool pty_backend_read_byte(PtyBackend *pty, uint8_t *byte_out)
{
    if (!pty || !pty->open || !byte_out) {
        return false;
    }

    uint8_t b = 0;
    ssize_t n = read(pty->master_fd, &b, 1);

    if (n == 1) {
        *byte_out = b;
        return true;
    }

    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return false;
    }

    return false;
}

bool pty_backend_write_byte(PtyBackend *pty, uint8_t byte)
{
    if (!pty || !pty->open) {
        return false;
    }

    ssize_t n = write(pty->master_fd, &byte, 1);

    if (n == 1) {
        return true;
    }

    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return false;
    }

    return false;
}