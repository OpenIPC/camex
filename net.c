/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * net.c — network transport (UDP / TCP), socket helpers
 *
 */

#define _GNU_SOURCE

#include "net.h"
#include "log.h"
#include "util.h"
#include "camex.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

int net_fd = -1;
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
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize)) < 0) {
        print_errno_message(LOG_WARNING, "setsockopt(SO_RCVBUF)");
    }
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize)) < 0) {
        print_errno_message(LOG_WARNING, "setsockopt(SO_SNDBUF)");
    }
    return set_fd_nonblocking(fd);
}

int net_configure_socket(int fd)
{
    if (current_config.bind_dev[0] != '\0') {
        if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE,
                       current_config.bind_dev,
                       strlen(current_config.bind_dev) + 1U) < 0) {
            print_errno_message(LOG_WARNING, "setsockopt(SO_BINDTODEVICE)");
        }
    }

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
}

int net_send_payload(int fd, const struct sockaddr_in *to,
                     const uint8_t *data, size_t len)
{
    int sent;

    if (fd < 0 || data == NULL) {
        return -1;
    }

    if (to == NULL) {
        sent = send(fd, data, len, 0);
    } else {
        sent = sendto(fd, data, len, 0,
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
    return net_send_payload(net_fd, NULL, data, len);
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

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        print_errno_message(LOG_WARNING, "setsockopt(SO_REUSEADDR)");
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

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        print_errno_message(LOG_ERR, "connect(TCP)");
        close(fd);
        return -1;
    }

    if (set_fd_nonblocking(fd) != 0) {
        close(fd);
        return -1;
    }

    server_addr = addr;
    return fd;
}

int net_tcp_accept(int listen_fd, struct sockaddr_in *peer)
{
    socklen_t addrlen = sizeof(*peer);
    int fd;

    fd = accept(listen_fd, (struct sockaddr *)peer, &addrlen);
    if (fd < 0) {
        return -1;
    }

    return fd;
}
