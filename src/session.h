/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * session.h — protocol v2 wire format and transport sessions (#17)
 *
 * Every packet (UDP datagram or TCP frame) starts with the magic "CXv2"
 * and a type byte:
 *
 *   INIT  client -> server  | sender_index 4 | Noise message 1 | mac1 16
 *   RESP  server -> client  | sender_index 4 | receiver_index 4 | message 2
 *   DATA  both ways         | receiver_index 4 | counter 8 LE | AEAD
 *
 * Indexes are big-endian random numbers chosen by each side for its own
 * end of a session; a DATA packet carries the index of the receiver's
 * end. DATA authenticates its 17-byte header as associated data. An empty
 * DATA payload is a keepalive. mac1 (keyed with the server's public key)
 * lets the server drop INIT packets from anyone who does not know that key
 * before any public-key work.
 *
 */

#ifndef CAMEX_SESSION_H
#define CAMEX_SESSION_H

#include "noise.h"

#include <stdint.h>
#include <stddef.h>
#include <time.h>

#define CAMEX_MAGIC        "CXv2"
#define CAMEX_PROTO_VER    2U
#define CAMEX_NOISE_PROLOGUE "camex v2"

#define V2_TYPE_INIT 1U
#define V2_TYPE_RESP 2U
#define V2_TYPE_DATA 3U

#define V2_INIT_HDR 9U    /* magic, type, sender */
#define V2_MAC_LEN 16U    /* mac1 at the end of INIT */
#define V2_RESP_HDR 13U   /* magic, type, sender, receiver */
#define V2_DATA_HDR 17U   /* magic, type, receiver, counter */
#define V2_OVERHEAD (V2_DATA_HDR + NOISE_TAGLEN)

#define V2_TIMESTAMP_LEN 12U       /* TAI64N in message 1 */
#define V2_REPLAY_WINDOW 2048U
#define V2_REKEY_AFTER_TIME 600    /* seconds */
#define V2_REKEY_AFTER_MSGS ((uint64_t)1 << 32)
#define V2_REJECT_AFTER_MSGS ((uint64_t)1 << 60)
#define V2_PREV_SESSION_GRACE 30   /* seconds an old session still receives */
#define V2_HANDSHAKE_RETRY 5       /* seconds between INIT retransmissions */
#define V2_REKEY_TIMEOUT 90        /* a rekey unanswered this long: reconnect */

typedef struct {
    uint64_t max;
    uint64_t bits[V2_REPLAY_WINDOW / 64U];
    uint8_t any;
} replay_window_t;

typedef struct {
    uint32_t local_index;    /* our end: peers send DATA with this index */
    uint32_t remote_index;   /* the peer's end: our DATA carries it */
    uint8_t k_send[NOISE_KEYLEN];
    uint8_t k_recv[NOISE_KEYLEN];
    uint64_t send_n;
    replay_window_t replay;
    time_t created;
    uint8_t valid;
} session_t;

/* Packet header helpers; return the type, or 0 if not a v2 packet */
uint8_t v2_packet_type(const uint8_t *pkt, size_t len);
uint32_t v2_read_u32(const uint8_t *p);
void v2_write_u32(uint8_t *p, uint32_t v);

/* Replay window (RFC 6479 style): 0 if n is new and inside the window.
 * replay_check() does not record; replay_record() does. */
int replay_check(const replay_window_t *r, uint64_t n);
void replay_record(replay_window_t *r, uint64_t n);

/* A new session from Split() keys */
void session_init(session_t *s, uint32_t local_index, uint32_t remote_index,
                  const uint8_t k_send[NOISE_KEYLEN],
                  const uint8_t k_recv[NOISE_KEYLEN], time_t now);
void session_wipe(session_t *s);

/* DATA packet with plain (len may be 0: keepalive). Returns the packet
 * length, or 0 (session invalid, counter exhausted, buffer too small). */
size_t session_seal(session_t *s, const uint8_t *plain, size_t len,
                    uint8_t *out, size_t out_size);

/* Opens a DATA packet for this session: header, replay window, AEAD.
 * Returns 0 and the payload, or -1. */
int session_open(session_t *s, const uint8_t *pkt, size_t len,
                 uint8_t *plain, size_t plain_size, size_t *plain_len);

/* Receiver index of a DATA packet */
uint32_t v2_data_receiver(const uint8_t *pkt);

/* TAI64N timestamp of now; compare: <0, 0, >0 */
void v2_timestamp(uint8_t out[V2_TIMESTAMP_LEN]);
/* The Unix seconds of a TAI64N timestamp of v2_timestamp() */
uint64_t v2_timestamp_seconds(const uint8_t ts[V2_TIMESTAMP_LEN]);
int v2_timestamp_cmp(const uint8_t a[V2_TIMESTAMP_LEN],
                     const uint8_t b[V2_TIMESTAMP_LEN]);

/* Random nonzero session index, or 0 if the random source failed */
uint32_t v2_new_index(void);

/* mac1 of an INIT packet (everything before the mac):
 * BLAKE2b-128 keyed with BLAKE2b-256("mac1----" || server public key) */
void v2_mac1(const uint8_t server_pub[32], const uint8_t *pkt, size_t len,
             uint8_t out[V2_MAC_LEN]);

#endif /* CAMEX_SESSION_H */
