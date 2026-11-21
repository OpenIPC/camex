/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * l2.h — Ethernet (TAP) networks, built with CAMEX_L2=1
 *
 * A network with mode = tap carries Ethernet frames. Inside a DATA packet
 * every frame starts with a type byte:
 *
 *   L2_WHOLE   0x00 | the frame
 *   L2_FRAG    0x01 | id 2 (big-endian) | index 1 | count 1 | a piece
 *
 * A frame longer than L2_PIECE goes in pieces of L2_PIECE bytes (the last
 * one shorter), so a 1514-byte frame of an MTU 1500 LAN never needs an IP
 * fragment on the way: IP fragments are what NATs and mobile networks
 * drop. The pieces of one frame share an id; a receiver keeps one frame
 * in assembly per sender and drops it when another id starts or after
 * L2_REASM_TIMEOUT seconds.
 *
 * The server is the switch of a TAP network: it learns which client a
 * MAC is behind, sends a frame to that client only, and floods broadcast,
 * multicast and unknown destinations to the network's other clients. It
 * is also the network's gateway: it answers ARP for the gateway address
 * with the gateway MAC, and takes IPv4 frames to that MAC to its tunnel
 * interface; packets from there to a client go as frames to the client's
 * MAC. MACs are derived (locally administered), so nobody has to learn
 * the gateway's or a client's own.
 *
 */

#ifndef CAMEX_L2_H
#define CAMEX_L2_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

#define L2_WHOLE 0x00U
#define L2_FRAG  0x01U
#define L2_FRAG_HDR 5U
#define L2_PIECE 1300U          /* bytes of a frame in one piece */
#define L2_MAX_PIECES 8U
#define L2_FRAME_MAX (L2_PIECE * L2_MAX_PIECES)
#define L2_REASM_TIMEOUT 3      /* seconds */
#define L2_ETH_HDR 14U

/* One frame in assembly */
typedef struct {
    uint16_t id;
    uint8_t count;
    uint8_t got;             /* bit i: piece i is in */
    size_t last_len;         /* length of the last piece */
    time_t started;
    uint8_t active;
    uint8_t buf[L2_FRAME_MAX];
} l2_reasm_t;

/* Send callback: one plaintext DATA payload; 0 or -1 */
typedef int (*l2_send_fn)(void *ctx, const uint8_t *payload, size_t len);

/* Frame -> one or more payloads through send. 0, or -1 (too long, or a
 * send failed: the rest is not sent) */
int l2_send_frame(const uint8_t *frame, size_t len, l2_send_fn send,
                  void *ctx);

/* A received payload. A whole frame: *frame points into payload (or into
 * r's buffer when this piece completed one), returns its length; 0 when
 * a frame is not complete yet; -1 for a malformed payload. r may be NULL
 * when no pieces are expected (then pieces are dropped). */
long l2_receive(l2_reasm_t *r, const uint8_t *payload, size_t len,
                const uint8_t **frame, time_t now);

/* MACs: the gateway of a network, a client of a network (locally
 * administered, unicast) */
void l2_gateway_mac(const uint8_t net_id[32], uint8_t mac[6]);
void l2_client_mac(const uint8_t net_id[32], const char *client_id,
                   uint8_t mac[6]);

/* "aa:bb:cc:dd:ee:ff" */
void l2_mac_text(const uint8_t mac[6], char *buf, size_t size);
int l2_mac_parse(const char *text, uint8_t mac[6]);

/* MAC table of the server's switch: the client slot (+1) a MAC of network
 * net1 was last seen behind */
void l2_mac_learn(uint16_t net1, const uint8_t mac[6], uint32_t slot1,
                  time_t now);
uint32_t l2_mac_lookup(uint16_t net1, const uint8_t mac[6], time_t now);
void l2_mac_forget_slot(uint32_t slot1);
void l2_mac_clear(void);

/* An ARP request for ip_be: the reply from mac (frame of 42 bytes in out),
 * or 0 if the frame is not such a request */
size_t l2_arp_reply(const uint8_t *frame, size_t len, uint32_t ip_be,
                    const uint8_t mac[6], uint8_t *out, size_t out_size);

#endif /* CAMEX_L2_H */
