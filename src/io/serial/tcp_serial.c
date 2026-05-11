#include "io/serial/tcp_serial.h"

#include <string.h>

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)

#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <fcntl.h>

#endif

static void tcp_serial_reset(
    TCPSerial *tcp)
{
    if (!tcp) {
        return;
    }

    tcp->listen_fd = -1;
    tcp->client_fd = -1;

    tcp->port = 0;

    tcp->server_open = false;
    tcp->client_connected = false;
}

static bool set_nonblocking(int fd)
{
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)

    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) {
        return false;
    }

    if (fcntl(fd,
              F_SETFL,
              flags | O_NONBLOCK) < 0)
    {
        return false;
    }

    return true;

#else
    (void)fd;
    return false;
#endif
}

static void tcp_serial_accept_client(
    TCPSerial *tcp)
{
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)

    if (!tcp ||
        !tcp->server_open ||
        tcp->client_connected)
    {
        return;
    }

    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);

    int fd =
        accept(tcp->listen_fd,
               (struct sockaddr *)&addr,
               &len);

    if (fd < 0) {

        if (errno == EAGAIN ||
            errno == EWOULDBLOCK)
        {
            return;
        }

        return;
    }

    set_nonblocking(fd);

    tcp->client_fd = fd;
    tcp->client_connected = true;

#else
    (void)tcp;
#endif
}

bool tcp_serial_open(TCPSerial *tcp,
                     uint16_t port)
{
    if (!tcp) {
        return false;
    }

    tcp_serial_reset(tcp);

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)

    int fd =
        socket(AF_INET,
               SOCK_STREAM,
               0);

    if (fd < 0) {
        return false;
    }

    int yes = 1;

    setsockopt(fd,
               SOL_SOCKET,
               SO_REUSEADDR,
               &yes,
               sizeof(yes));

    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd,
             (struct sockaddr *)&addr,
             sizeof(addr)) < 0)
    {
        close(fd);
        return false;
    }

    if (listen(fd, 1) < 0) {
        close(fd);
        return false;
    }

    if (!set_nonblocking(fd)) {
        close(fd);
        return false;
    }

    tcp->listen_fd = fd;
    tcp->port = port;

    tcp->server_open = true;

    return true;

#else
    (void)port;
    return false;
#endif
}

void tcp_serial_close(TCPSerial *tcp)
{
    if (!tcp) {
        return;
    }

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)

    if (tcp->client_fd >= 0) {
        close(tcp->client_fd);
    }

    if (tcp->listen_fd >= 0) {
        close(tcp->listen_fd);
    }

#endif

    tcp_serial_reset(tcp);
}

bool tcp_serial_is_open(
    const TCPSerial *tcp)
{
    return tcp &&
           tcp->server_open;
}

bool tcp_serial_client_connected(
    const TCPSerial *tcp)
{
    return tcp &&
           tcp->client_connected;
}

bool tcp_serial_read_byte(
    TCPSerial *tcp,
    uint8_t *byte_out)
{
    if (!tcp ||
        !byte_out)
    {
        return false;
    }

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)

    tcp_serial_accept_client(tcp);

    if (!tcp->client_connected) {
        return false;
    }

    uint8_t b;

    ssize_t n =
        recv(tcp->client_fd,
             &b,
             1,
             0);

    if (n == 1) {
        *byte_out = b;
        return true;
    }

    if (n == 0) {

        close(tcp->client_fd);

        tcp->client_fd = -1;
        tcp->client_connected = false;

        return false;
    }

    if (errno == EAGAIN ||
        errno == EWOULDBLOCK)
    {
        return false;
    }

    close(tcp->client_fd);

    tcp->client_fd = -1;
    tcp->client_connected = false;

#endif

    return false;
}

bool tcp_serial_write_byte(
    TCPSerial *tcp,
    uint8_t byte)
{
    if (!tcp) {
        return false;
    }

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)

    tcp_serial_accept_client(tcp);

    if (!tcp->client_connected) {
        return false;
    }

    ssize_t n =
        send(tcp->client_fd,
             &byte,
             1,
             0);

    if (n == 1) {
        return true;
    }

    if (errno == EAGAIN ||
        errno == EWOULDBLOCK)
    {
        return false;
    }

    close(tcp->client_fd);

    tcp->client_fd = -1;
    tcp->client_connected = false;

#endif

    return false;
}