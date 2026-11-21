/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * mesh.h — clients of a network with mesh = yes talk to each other
 *          directly where NAT lets them, and through the server where it
 *          does not. Built with CAMEX_MESH.
 *
 * Every mesh client has a second UDP socket, for its peers. It sends an
 * ENDPOINT packet from it to the server (sealed with its session to the
 * server, so nobody else can claim it): the server learns the address
 * and port that socket has after the client's NAT. The client also tells
 * the server that socket's address on its own LAN.
 *
 * Traffic to another client of the network goes through the server at
 * first, as always. With it the client asks the server about that peer
 * (PEER?, a control message inside its session: a DATA payload starting
 * with 0xC0). The server answers both clients with each other's
 * endpoints and a fresh key for the pair (PEER). Both then send PUNCH
 * packets to each other's public and LAN endpoints at once, which opens
 * their NATs to each other; the first one that comes back authenticated
 * makes the path direct. From then on their packets go peer to peer (P2P
 * packets, sealed with the pair's keys, one key per direction).
 *
 * A direct path that stays silent for MESH_DEAD seconds goes back to the
 * server, and is tried again later; one that never opens (both behind
 * symmetric NATs, say) stays on the server and is tried again after a
 * longer while. Every 30 s each client reports its peers (STATS) for
 * camex --status and --top on the server.
 *
 * The server hands out the pair keys, so it could read what its clients
 * send each other; as in hub mode, the server is trusted.
 *
 *   ENDPOINT client -> server | receiver_index 4 | counter 8 | AEAD(empty)
 *   P2P      peer -> peer     | pair 4 | dir 1 | counter 8 | AEAD(payload)
 *
 * P2P payloads: 0xC1 'P'/'A' (a punch, its answer), anything else is
 * tunnel data (an IPv4 packet, or a TAP network's frame payload).
 *
 */

#ifndef CAMEX_MESH_H
#define CAMEX_MESH_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

#include "session.h"   /* V2_TYPE_ENDPOINT, V2_TYPE_P2P, V2_P2P_HDR */

#define MESH_CTRL 0xC0U           /* a control message in a session */
#define MESH_PUNCH 0xC1U          /* a punch in a P2P packet */

#define MESH_PEERS 64U
#define MESH_PUNCH_EVERY_MS 250
#define MESH_PUNCH_FOR 10         /* seconds of punching */
#define MESH_KEEPALIVE 15         /* a direct path's keepalives */
#define MESH_DEAD 35              /* a silent direct path goes back */
#define MESH_RETRY 60             /* a failed punch is tried again */
#define MESH_ASK_AGAIN 30         /* PEER? for one peer at most so often */
#define MESH_STATS_EVERY 30

/* Client */

/* Start mesh for this client (the server's CONFIG said MESH=1): its peer
 * socket. 0 or -1 */
int mesh_client_start(int l2);
void mesh_client_stop(void);
int mesh_client_active(void);
int mesh_fd(void);

/* After every new session with the server: the ENDPOINT packet, the LAN
 * address */
void mesh_session_up(void);

/* Tunnel data to dst (TUN: the IPv4 destination; TAP: the frame's
 * destination MAC, ip 0): 1 if it went peer to peer, 0 if it must go
 * through the server (and the server was asked about the peer), -1 on
 * an error */
int mesh_send(uint32_t dst_ip_be, const uint8_t *dst_mac,
              const uint8_t *payload, size_t len);

/* The peer socket is readable */
void mesh_readable(void);

/* A control message from the server (after MESH_CTRL) */
void mesh_control(const uint8_t *msg, size_t len);

/* Every second */
void mesh_tick(void);

/* "<peer> <state> <rx> <tx>" lines of this client (tests, logs) */
size_t mesh_describe(char *buf, size_t size);

/* Pair keys: the two directions from one key; a and b are the two client
 * IDs (the direction from the lower ID is 0) */
void mesh_pair_keys(const uint8_t key[32], int dir, uint8_t out[32]);

#endif /* CAMEX_MESH_H */
