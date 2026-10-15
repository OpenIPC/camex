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
#include "crypto.h"
#include "tun.h"

#include <errno.h>
#include <stdlib.h>
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

/*
 * Per-connection TCP send pending buffers.
 * When a non-blocking TCP send returns EAGAIN mid-frame, the unsent data
 * is saved here so the next call can resume transmission rather than
 * re-sending the 2-byte length header (which would desync the protocol).
 *
 * Each fd has its own slot, so a slow connection only delays itself and
 * never blocks sends to other connections. A slot buffer is allocated on
 * first use and kept for reuse; net_tcp_close() releases it. Pending data
 * must never outlive its socket: a stale entry would put a closed fd into
 * the select() write set (EBADF on every iteration) or leak the tail of an
 * old frame into a new connection that reuses the fd number.
 */
/* Largest frame: 2-byte length + encrypted packet of TUN_PACKET_MAX bytes */
#define TCP_SEND_BUF_SIZE (2U + CAMEX_HDR_LEN + 1U + TUN_PACKET_MAX + 16U)
#define TCP_PENDING_SLOTS (CAMEX_MAX_CLIENTS + 1U)

typedef struct {
    int fd;            /* owner fd, -1 = slot free */
    uint8_t *buf;      /* TCP_SEND_BUF_SIZE bytes, allocated on first use */
    size_t len;
    size_t offset;
} tcp_send_pending_t;

static tcp_send_pending_t tcp_pending[TCP_PENDING_SLOTS];
static int tcp_pending_ready = 0;

static void tcp_pending_init(void)
{
    size_t i;

    if (tcp_pending_ready) {
        return;
    }
    for (i = 0; i < TCP_PENDING_SLOTS; ++i) {
        tcp_pending[i].fd = -1;
        tcp_pending[i].buf = NULL;
        tcp_pending[i].len = 0;
        tcp_pending[i].offset = 0;
    }
    tcp_pending_ready = 1;
}

static tcp_send_pending_t *tcp_pending_find(int fd)
{
    size_t i;

    tcp_pending_init();
    for (i = 0; i < TCP_PENDING_SLOTS; ++i) {
        if (tcp_pending[i].fd == fd) {
            return &tcp_pending[i];
        }
    }
    return NULL;
}

/* Take a free slot for fd, preferring one that already has a buffer. */
static tcp_send_pending_t *tcp_pending_acquire(int fd)
{
    tcp_send_pending_t *slot = NULL;
    size_t i;

    tcp_pending_init();
    for (i = 0; i < TCP_PENDING_SLOTS; ++i) {
        if (tcp_pending[i].fd != -1) {
            continue;
        }
        if (tcp_pending[i].buf != NULL) {
            slot = &tcp_pending[i];
            break;
        }
        if (slot == NULL) {
            slot = &tcp_pending[i];
        }
    }
    if (slot == NULL) {
        return NULL;
    }
    if (slot->buf == NULL) {
        slot->buf = malloc(TCP_SEND_BUF_SIZE);
        if (slot->buf == NULL) {
            return NULL;
        }
    }
    slot->fd = fd;
    slot->len = 0;
    slot->offset = 0;
    return slot;
}

static void tcp_pending_release(tcp_send_pending_t *slot)
{
    slot->fd = -1;
    slot->len = 0;
    slot->offset = 0;
}

/*
 * Flush any pending TCP send data for the given fd.
 * Returns 0 when fully flushed, 1 if still pending (EAGAIN), -1 on error.
 */
static int tcp_send_flush_pending(int fd)
{
    tcp_send_pending_t *slot = tcp_pending_find(fd);

    if (fd < 0 || slot == NULL) {
        return 0;  /* nothing pending for this fd */
    }

    while (slot->offset < slot->len) {
        ssize_t n;
        size_t remaining = slot->len - slot->offset;

        n = send(fd, SEND_CAST slot->buf + slot->offset,
                 remaining, MSG_NOSIGNAL);
        if (n < 0) {
#ifdef _WIN32
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK || e == WSAEINTR) {
                return 1;  /* still pending */
            }
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                return 1;  /* still pending */
            }
#endif
            /* Hard error — discard pending */
            tcp_pending_release(slot);
            return -1;
        }
        slot->offset += (size_t)n;
    }

    /* Fully flushed */
    tcp_pending_release(slot);
    return 0;
}

/* Return an fd that has pending TCP send data, or -1 if none. */
int net_tcp_get_pending_fd(void)
{
    size_t i;

    tcp_pending_init();
    for (i = 0; i < TCP_PENDING_SLOTS; ++i) {
        if (tcp_pending[i].fd >= 0 &&
            tcp_pending[i].offset < tcp_pending[i].len) {
            return tcp_pending[i].fd;
        }
    }
    return -1;
}

/* Add every fd with pending send data to set; returns the new max fd. */
int net_tcp_pending_fdset(fd_set *set, int maxfd)
{
    size_t i;

    tcp_pending_init();
    for (i = 0; i < TCP_PENDING_SLOTS; ++i) {
        int fd = tcp_pending[i].fd;

        if (fd >= 0 && tcp_pending[i].offset < tcp_pending[i].len) {
            FD_SET(fd, set);
            if (fd > maxfd) {
                maxfd = fd;
            }
        }
    }
    return maxfd;
}

/* Flush pending data for every fd that select() reported writable. */
void net_tcp_flush_writable(fd_set *set)
{
    size_t i;

    tcp_pending_init();
    for (i = 0; i < TCP_PENDING_SLOTS; ++i) {
        int fd = tcp_pending[i].fd;

        if (fd >= 0 && FD_ISSET(fd, set)) {
            (void)tcp_send_flush_pending(fd);
        }
    }
}

/* Public wrapper: try to flush pending data for the given fd.
 * Returns 0 = fully flushed, 1 = still pending (EAGAIN), -1 = error. */
int net_tcp_flush_pending(int fd)
{
    return tcp_send_flush_pending(fd);
}

/* Drop any pending send data for fd and close it. */
void net_tcp_close(int fd)
{
    tcp_send_pending_t *slot;

    if (fd < 0) {
        return;
    }
    slot = tcp_pending_find(fd);
    if (slot != NULL) {
        tcp_pending_release(slot);
    }
    close(fd);
}

/* Cross-platform EAGAIN/EWOULDBLOCK check for socket operations */
int net_sock_err_is_again(void)
{
#ifdef _WIN32
    int e = WSAGetLastError();
    return (e == WSAEWOULDBLOCK || e == WSAEINTR);
#else
    return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
#endif
}

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

    memcpy(addr, result->ai_addr, result->ai_addrlen);
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
        net_tcp_close(net_fd);
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
    size_t frame_size;
    tcp_send_pending_t *slot;

    if (fd < 0 || data == NULL || len > (TCP_SEND_BUF_SIZE - 2U)) {
        return -1;
    }

    /* Flush any pending data for this fd first */
    {
        int ret = tcp_send_flush_pending(fd);
        if (ret != 0) {
            return ret;  /* 1 = still pending (EAGAIN), -1 = error */
        }
    }

    /* Build the frame in this fd's own pending slot */
    slot = tcp_pending_acquire(fd);
    if (slot == NULL) {
        /* No free slot or out of memory: the frame is dropped, the stream
         * stays intact.  Report it as an error, not as backpressure, so
         * the caller counts the loss instead of waiting for a flush. */
        return -1;
    }
    net_len = htons((uint16_t)len);
    memcpy(header, &net_len, 2);
    memcpy(slot->buf, header, 2);
    memcpy(slot->buf + 2, data, len);
    frame_size = 2 + len;
    slot->len = frame_size;
    slot->offset = 0;

    /* Try to send immediately; flush will handle EAGAIN mid-write */
    return tcp_send_flush_pending(fd);
}

/*
 * TCP recv helpers: use platform-specific EAGAIN via net_sock_err_is_again().
 * Returns:  0 = full frame received, *len set, state reset for next frame
 *           1 = partial data / EAGAIN (state preserved, caller re-selects)
 *          -1 = hard error (connection closed, protocol error)
 *
 * BUGFIX (10.09): partial-read progress now lives in *state (owned by the
 * caller, one instance per TCP connection) instead of a function-local
 * counter that reset to 0 every call — see tcp_recv_state_t in net.h for
 * why that was corrupting the frame stream under any real traffic burst.
 */
int net_tcp_recv_frame(int fd, uint8_t *buffer, size_t size, size_t *len,
                       tcp_recv_state_t *state)
{
    ssize_t n;

    if (fd < 0 || buffer == NULL || len == NULL || state == NULL) {
        return -1;
    }

    if (!state->header_done) {
        while (state->header_have < 2) {
            n = read(fd, state->header + state->header_have,
                     2 - state->header_have);
            if (n < 0) {
                if (net_sock_err_is_again()) {
                    return 1;  /* caller will re-select; state preserved */
                }
                return -1;
            }
            if (n == 0) {
                return -1;  /* connection closed */
            }
            state->header_have += (size_t)n;
        }

        {
            uint16_t net_len;
            memcpy(&net_len, state->header, 2);
            state->body_len = (size_t)ntohs(net_len);
        }

        if (state->body_len == 0U || state->body_len > size) {
            log_message(LOG_ERR, "net_tcp_recv_frame: bad body_len=%zu (size cap=%zu), header bytes=%02x %02x, fd=%d",
                        state->body_len, size, state->header[0], state->header[1], fd);
            /* Stream is desynced beyond recovery for this connection —
             * caller treats this as a hard error and tears the connection
             * down, so no point preserving state further. */
            return -1;
        }

        state->header_done = 1;
        state->body_have = 0;
    }

    while (state->body_have < state->body_len) {
        n = read(fd, buffer + state->body_have,
                 state->body_len - state->body_have);
        if (n < 0) {
            if (net_sock_err_is_again()) {
                return 1;  /* caller will re-select; state preserved */
            }
            return -1;
        }
        if (n == 0) {
            return -1;  /* connection closed */
        }
        state->body_have += (size_t)n;
    }

    *len = state->body_len;

    /* Frame complete — reset state so the NEXT call starts a fresh header,
     * at the correct stream position (right after this frame's last byte,
     * since we only ever read() exactly what each phase needed). */
    state->header_have = 0;
    state->header_done = 0;
    state->body_len = 0;
    state->body_have = 0;
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
