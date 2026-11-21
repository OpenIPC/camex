/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * pake.h — CPace over X25519: a client proves to the server, and the
 *          server to the client, that both know the network's name,
 *          without anything an attacker could test offline (networks.md)
 *
 *   PAKE1 client -> server | sid 16 | Ya 32 | zeros to V2_PAKE1_LEN
 *   PAKE2 server -> client | sid 16 | flags 1 | count 1 | count x (Yb 32,
 *                            tag 16)
 *   PAKE3 client -> server | sid 16 | index 1 | tag 16
 *
 * The generator of a network is Elligator2 of BLAKE2b("camex cpace v1" ||
 * network ID || sid); the server answers once for each network it knows,
 * the client finds its own by the tag. The shared secret K gives the
 * pre-shared key of the Noise handshake that follows, so only a server
 * that knows the name can complete it, whatever key it presents.
 *
 */

#ifndef CAMEX_PAKE_H
#define CAMEX_PAKE_H

#include <stdint.h>
#include <stddef.h>

#define PAKE_SID_LEN 16U
#define PAKE_TAG_LEN 16U
#define PAKE_ENTRY_LEN (32U + PAKE_TAG_LEN)
#define PAKE_MAX_NETS 24U          /* entries in PAKE2: it stays < PAKE1 */
#define PAKE_FLAG_OPEN 0x01U       /* the server takes new networks */
#define PAKE_FLAG_NAME 0x02U       /* and shows them by the name REGISTER's
                                    * NAME gives */

/* The generator of network id for session sid */
void pake_generator(const uint8_t id[32], const uint8_t sid[PAKE_SID_LEN],
                    uint8_t g[32]);

/* scalar * point; -1 for a low-order (all-zero) result */
int pake_mult(uint8_t out[32], const uint8_t scalar[32],
              const uint8_t point[32]);

/* The server's tag (who 's') or the client's ('c') of K over the
 * session: sid, Ya, Yb and the server's public key */
void pake_tag(uint8_t out[PAKE_TAG_LEN], char who, const uint8_t k[32],
              const uint8_t sid[PAKE_SID_LEN], const uint8_t ya[32],
              const uint8_t yb[32], const uint8_t server_pub[32]);

/* The Noise pre-shared key from K */
void pake_psk(uint8_t psk[32], const uint8_t k[32]);

#endif /* CAMEX_PAKE_H */
