/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * net.h — network transport (UDP / TCP), socket helpers
 *
 */

#ifndef CAMEX_NET_H
#define CAMEX_NET_H

#include <stdint.h>
#include <stddef.h>

#ifndef _WIN32
#include <netinet/in.h>
#else
#include <winsock2.h>
#endif

/* Transport type */
typedef enum {
    CAMEX_TRANSPORT_UDP = 0,
    CAMEX_TRANSPORT_TCP = 1
} camex_transport_t;

/* Network FD (global) */
extern int net_fd;       /* client-side connected / server-side data */
extern int listen_fd;    /* TCP listen socket (server mode, -1 when unused) */
extern struct sockaddr_in server_addr;

/* Resolve endpoint (socktype: SOCK_DGRAM or SOCK_STREAM) */
int net_resolve_endpoint(const char *host, int port,
                         struct sockaddr_in *addr, const char *label,
                         int socktype);

/* Compare sockaddr_in */
int net_sockaddr_equal(const struct sockaddr_in *lhs,
                       const struct sockaddr_in *rhs);

/* Format sockaddr_in to "ip:port" string */
void net_sockaddr_to_string(const struct sockaddr_in *addr,
                            char *buffer, size_t size);

/* Create a generic socket (AF_INET, SOCK_DGRAM/SOCK_STREAM) */
int net_create_socket(camex_transport_t transport);

/* Configure socket (buffer sizes, nonblocking, bind-dev) */
int net_configure_socket(int fd);

/* Open UDP socket (default 4MB buffers, nonblocking) */
int net_open_udp_socket(void);

/* Tune UDP buffer sizes */
int net_tune_udp_socket(int fd);

/* Close net_fd */
void net_close(void);

/* Send raw payload */
int net_send_payload(int fd, const struct sockaddr_in *to,
                     const uint8_t *data, size_t len);
int net_send_text(int fd, const struct sockaddr_in *to,
                  const char *text);

/* Send with explicit connected-socket (client) */
int net_client_send(const uint8_t *data, size_t len);

/*
 * TCP-specific helpers (used when transport == CAMEX_TRANSPORT_TCP)
 */
int net_tcp_listen(const char *bind_ip, int port);
int net_tcp_connect(const char *host, int port);
int net_tcp_accept(int listen_fd, struct sockaddr_in *peer);

/* TCP framing: send 2-byte big-endian length prefix + payload */
int net_tcp_send_frame(int fd, const uint8_t *data, size_t len);

/* TCP framing: recv 2-byte length prefix + payload (blocks until full frame) */
int net_tcp_recv_frame(int fd, uint8_t *buffer, size_t size, size_t *len);

/* Cross-platform EAGAIN/EWOULDBLOCK check for socket operations.
 * On POSIX: checks errno.  On Windows: checks WSAGetLastError(). */
int net_sock_err_is_again(void);

#endif /* CAMEX_NET_H */
