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

/*
 * Persistent partial-read state for TCP frame reception.
 * BUGFIX (10.09): net_tcp_recv_frame() previously used a purely local
 * byte-counter that reset to 0 on every call. If a read() returned EAGAIN
 * partway through either the 2-byte length header or the frame body (which
 * WILL happen under any real burst of traffic — TCP segment boundaries
 * don't line up with logical frame boundaries), the NEXT call restarted
 * "expect a fresh 2-byte header" at whatever stream position it happened
 * to be at — usually mid-body of the frame that got interrupted — reading
 * garbage as a length prefix and desyncing the whole connection. This
 * struct must be OWNED PER-CONNECTION by the caller (one instance per TCP
 * client on the server side, one global instance on the client side) and
 * passed to every net_tcp_recv_frame() call for that connection so partial
 * progress survives across EAGAIN. Zero-initialize on connection accept
 * or client (re)connect — never reuse across a different connection's
 * bytes.
 */
typedef struct {
    uint8_t header[2];
    size_t header_have;
    int header_done;
    size_t body_len;
    size_t body_have;
} tcp_recv_state_t;

/* TCP framing: recv 2-byte length prefix + payload.
 * state: this connection's persistent partial-read tracker — see
 * tcp_recv_state_t above. Returns 0 on a complete frame (resets state for
 * the next one), 1 on EAGAIN/partial (state preserved, call again once
 * readable), -1 on hard error/close. */
int net_tcp_recv_frame(int fd, uint8_t *buffer, size_t size, size_t *len,
                       tcp_recv_state_t *state);

/* Return fd that has pending TCP send data, or -1 if none (for select() writefds) */
int net_tcp_get_pending_fd(void);

/* Try to flush pending TCP send data for the given fd */
int net_tcp_flush_pending(int fd);

/* Cross-platform EAGAIN/EWOULDBLOCK check for socket operations.
 * On POSIX: checks errno.  On Windows: checks WSAGetLastError(). */
int net_sock_err_is_again(void);

#endif /* CAMEX_NET_H */
