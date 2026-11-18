/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * tcpgate.h — who may take a new TCP connection that comes in through the
 * tunnel (#187)
 *
 * On Android several apps embed the tunnel library and one VPN runs at a time:
 * a connection from the server to this client's address goes to whichever app
 * listens on the port, not only to the app that owns the tunnel. With a gate
 * set (the library's JNI sets one), a SYN from the server is passed to the tun
 * only when the gate accepts it; a refused one gets a TCP RST back through the
 * tunnel. Without a gate (camex on cameras and routers) nothing changes.
 */
#ifndef CAMEX_TCPGATE_H
#define CAMEX_TCPGATE_H

#include <stddef.h>
#include <stdint.h>

/* An IPv4 RST: header and TCP header, no options */
#define TCPGATE_RST_LEN 40U

/* The gate: addresses in network byte order, ports in host order.
 * Returns nonzero to let the connection through. */
typedef int (*tcpgate_fn)(uint32_t dst_ip, uint16_t dst_port,
                          uint32_t src_ip, uint16_t src_port);

/* Sets the gate (NULL: none, every packet passes). */
void tcpgate_set(tcpgate_fn fn);

/* Whether a gate is set. */
int tcpgate_active(void);

/* A packet from the server on its way to the tun. Returns 1 if it passes: not
 * IPv4 TCP, no gate, not a SYN (without ACK), or the gate accepts it. Returns 0
 * if it is dropped: TCP in fragments or with a broken header (no RST), or a SYN
 * the gate refused; then *rst_len is the length of the RST built in rst
 * (TCPGATE_RST_LEN, or 0 if rst_cap is too small), to send back to the server. */
int tcpgate_check(const uint8_t *pkt, size_t len,
                  uint8_t *rst, size_t rst_cap, size_t *rst_len);

#endif /* CAMEX_TCPGATE_H */
