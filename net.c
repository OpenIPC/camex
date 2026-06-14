/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * net.c — network transport (UDP / TCP), socket helpers
 *
 */

#include "net.h"
#include "log.h"
#include "util.h"
#include "camex.h"

#include <errno.h>
#include <string.h>
#include <stdio.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#ifdef _WIN32
#define close(fd) closesocket(fd)
/* On Windows, read() does not work on SOCKET handles */
#define read(s, buf, len) recv(s, (char *)(buf), (len), 0)
/* Winsock setsockopt/send/sendto expect const char* */
#define OPTVAL_CAST (const char *)
#define SEND_CAST   (const char *)
#else
#define OPTVAL_CAST
#define SEND_CAST
#endif

int net_fd = -1;
int listen_fd = -1;
struct sockaddr_in server_addr;

int net_resolve_endpoint(const char *host, int port,
                         struct sockaddr_in *addr, const char *label,
                         int socktype)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    char portbuf[16];

    if (host == NULL || addr == NULL || label == NULL) {
        return -1;
    }

    memset(addr, 0, sizeof(*addr));
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = socktype;
    snprintf(portbuf, sizeof(portbuf), "%d", port);

    if (getaddrinfo(host, portbuf, &hints, &result) != 0 || result == NULL) {
        log_message(LOG_ERR, "Cannot resolve %s: %s", label, host);
        return -1;
    }

    memcpy(addr, result->ai_addr, sizeof(*addr));
    freeaddrinfo(result);
    return 0;
}

int net_sockaddr_equal(const struct sockaddr_in *lhs,
                       const struct sockaddr_in *rhs)
{
    return lhs != NULL && rhs != NULL &&
           lhs->sin_family == rhs->sin_family &&
           lhs->sin_port == rhs->sin_port &&
           lhs->sin_addr.s_addr == rhs->sin_addr.s_addr;
}

void net_sockaddr_to_string(const struct sockaddr_in *addr,
                            char *buffer, size_t size)
{
    char ipbuf[INET_ADDRSTRLEN];
    const char *ip;

    if (buffer == NULL || size == 0U) {
        return;
    }

    if (addr == NULL) {
        snprintf(buffer, size, "(null)");
        return;
    }

    ip = inet_ntop(AF_INET, &addr->sin_addr, ipbuf, sizeof(ipbuf));
    if (ip == NULL) {
        snprintf(buffer, size, "?:%u", (unsigned)ntohs(addr->sin_port));
        return;
    }

    snprintf(buffer, size, "%s:%u", ip, (unsigned)ntohs(addr->sin_port));
}

int net_create_socket(camex_transport_t transport)
{
    int type = (transport == CAMEX_TRANSPORT_TCP) ? SOCK_STREAM : SOCK_DGRAM;
    return socket(AF_INET, type, 0);
}

int net_tune_udp_socket(int fd)
{
    int bufsize;

    bufsize = 4 * 1024 * 1024;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, OPTVAL_CAST &bufsize, sizeof(bufsize)) < 0) {
        print_errno_message(LOG_WARNING, "setsockopt(SO_RCVBUF)");
    }
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, OPTVAL_CAST &bufsize, sizeof(bufsize)) < 0) {
        print_errno_message(LOG_WARNING, "setsockopt(SO_SNDBUF)");
    }
    return set_fd_nonblocking(fd);
}

int net_configure_socket(int fd)
{
#ifdef __linux__
    if (current_config.bind_dev[0] != '\0') {
        if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE,
                       current_config.bind_dev,
                       strlen(current_config.bind_dev) + 1U) < 0) {
            print_errno_message(LOG_WARNING, "setsockopt(SO_BINDTODEVICE)");
        }
    }
#endif

    if (net_tune_udp_socket(fd) != 0) {
        print_errno_message(LOG_ERR, "fcntl(O_NONBLOCK)");
        return -1;
    }

    return 0;
}

int net_open_udp_socket(void)
{
    int fd;

    fd = net_create_socket(CAMEX_TRANSPORT_UDP);
    if (fd < 0) {
        print_errno_message(LOG_ERR, "socket");
        return -1;
    }

    if (net_configure_socket(fd) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

void net_close(void)
{
    if (net_fd >= 0) {
        close(net_fd);
        net_fd = -1;
    }
    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }
}

int net_send_payload(int fd, const struct sockaddr_in *to,
                     const uint8_t *data, size_t len)
{
    int sent;

    if (fd < 0 || data == NULL) {
        return -1;
    }

    if (to == NULL) {
        sent = send(fd, SEND_CAST data, len, 0);
    } else {
        sent = sendto(fd, SEND_CAST data, len, 0,
                      (const struct sockaddr *)to, sizeof(*to));
    }

    if (sent < 0 || (size_t)sent != len) {
        return -1;
    }

    return 0;
}

int net_send_text(int fd, const struct sockaddr_in *to, const char *text)
{
    return net_send_payload(fd, to, (const uint8_t *)text, strlen(text));
}

int net_client_send(const uint8_t *data, size_t len)
{
    if (current_config.transport == CAMEX_TRANSPORT_TCP) {
        return net_tcp_send_frame(net_fd, data, len);
    }
    return net_send_payload(net_fd, NULL, data, len);
}

int net_tcp_send_frame(int fd, const uint8_t *data, size_t len)
{
    uint8_t header[2];
    uint16_t net_len;
    size_t total = 0, frame_size;

    if (fd < 0 || data == NULL || len > 65535U) {
        return -1;
    }

    net_len = htons((uint16_t)len);
    memcpy(header, &net_len, 2);
    frame_size = 2 + len;

    /*
     * Coalesce header + data and loop on send() to handle
     * partial writes on non-blocking TCP sockets.
     */
    while (total < frame_size) {
        ssize_t n;
        size_t offset = total;
        size_t remaining = frame_size - total;

        /* Choose which part of the frame to send */
        if (offset < 2) {
            size_t hdr_rem = 2 - offset;
            size_t chunk = (hdr_rem < remaining) ? hdr_rem : remaining;
            n = send(fd, SEND_CAST header + offset, chunk, MSG_NOSIGNAL);
        } else {
            n = send(fd, SEND_CAST data + (offset - 2), remaining, MSG_NOSIGNAL);
        }

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* caller will re-select after EPOLLOUT */
                return -1;
            }
            return -1;
        }
        total += (size_t)n;
    }
    return 0;
}

int net_tcp_recv_frame(int fd, uint8_t *buffer, size_t size, size_t *len)
{
    uint8_t header[2];
    uint16_t net_len;
    size_t body_len;
    ssize_t n;
    size_t total = 0;

    if (fd < 0 || buffer == NULL || len == NULL) {
        return -1;
    }

    /* Read 2-byte header */
    while (total < 2) {
        n = read(fd, header + total, 2 - total);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return -1;  /* caller will re-select */
            }
            return -1;
        }
        if (n == 0) {
            errno = ENOTCONN;
            return -1;  /* connection closed */
        }
        total += (size_t)n;
    }

    memcpy(&net_len, header, 2);
    body_len = (size_t)ntohs(net_len);

    if (body_len == 0U || body_len > size) {
        return -1;
    }

    total = 0;
    while (total < body_len) {
        n = read(fd, buffer + total, body_len - total);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return -1;  /* caller will re-select */
            }
            return -1;
        }
        if (n == 0) {
            errno = ENOTCONN;
            return -1;  /* connection closed */
        }
        total += (size_t)n;
    }

    *len = body_len;
    return 0;
}

/*
 * TCP-specific helpers
 */
int net_tcp_listen(const char *bind_ip, int port)
{
    struct sockaddr_in addr;
    int reuse = 1;
    int fd;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        print_errno_message(LOG_ERR, "socket(TCP)");
        return -1;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, OPTVAL_CAST &reuse, sizeof(reuse)) < 0) {
        print_errno_message(LOG_WARNING, "setsockopt(SO_REUSEADDR)");
    }

#if defined(__APPLE__)
    {
        int nosigpipe = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
    }
#endif

    if (net_configure_socket(fd) != 0) {
        close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind_ip != NULL && *bind_ip != '\0' &&
        inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) {
        log_message(LOG_ERR, "Invalid bind IP: %s", bind_ip);
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        print_errno_message(LOG_ERR, "bind(TCP)");
        close(fd);
        return -1;
    }

    if (listen(fd, 16) < 0) {
        print_errno_message(LOG_ERR, "listen(TCP)");
        close(fd);
        return -1;
    }

    return fd;
}

int net_tcp_connect(const char *host, int port)
{
    struct sockaddr_in addr;
    int fd;

    if (net_resolve_endpoint(host, port, &addr, "server",
                             SOCK_STREAM) != 0) {
        return -1;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        print_errno_message(LOG_ERR, "socket(TCP)");
        return -1;
    }

#if defined(__APPLE__)
    {
        int nosigpipe = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
    }
#endif

    /* Set bind_dev and buffer sizes before connect */
#ifdef __linux__
    if (current_config.bind_dev[0] != '\0') {
        if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE,
                       current_config.bind_dev,
                       strlen(current_config.bind_dev) + 1U) < 0) {
            print_errno_message(LOG_WARNING,
                                "setsockopt(SO_BINDTODEVICE)");
        }
    }
#endif
    {
        int bufsize = 4 * 1024 * 1024;
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                         OPTVAL_CAST &bufsize, sizeof(bufsize));
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF,
                         OPTVAL_CAST &bufsize, sizeof(bufsize));
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        print_errno_message(LOG_ERR, "connect(TCP)");
        close(fd);
        return -1;
    }

    /* Set nonblocking after connect completes */
    if (set_fd_nonblocking(fd) != 0) {
        close(fd);
        return -1;
    }

    server_addr = addr;
    return fd;
}

int net_tcp_accept(int lfd, struct sockaddr_in *peer)
{
    socklen_t addrlen = sizeof(*peer);
    int fd;

    fd = accept(lfd, (struct sockaddr *)peer, &addrlen);
    if (fd < 0) {
        return -1;
    }

    return fd;
}
