/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * pake.c — CPace over X25519 (pake.h)
 *
 */

#include "pake.h"
#include "monocypher.h"

#include <string.h>

void pake_generator(const uint8_t id[32], const uint8_t sid[PAKE_SID_LEN],
                    uint8_t g[32])
{
    static const char ctx[] = "camex cpace v1";
    crypto_blake2b_ctx c;
    uint8_t h[32];

    crypto_blake2b_init(&c, sizeof(h));
    crypto_blake2b_update(&c, (const uint8_t *)ctx, sizeof(ctx) - 1U);
    crypto_blake2b_update(&c, id, 32U);
    crypto_blake2b_update(&c, sid, PAKE_SID_LEN);
    crypto_blake2b_final(&c, h);
    crypto_elligator_map(g, h);
    crypto_wipe(h, sizeof(h));
}

int pake_mult(uint8_t out[32], const uint8_t scalar[32],
              const uint8_t point[32])
{
    static const uint8_t zero[32];

    crypto_x25519(out, scalar, point);
    return crypto_verify32(out, zero) == 0 ? -1 : 0;
}

void pake_tag(uint8_t out[PAKE_TAG_LEN], char who, const uint8_t k[32],
              const uint8_t sid[PAKE_SID_LEN], const uint8_t ya[32],
              const uint8_t yb[32], const uint8_t server_pub[32])
{
    crypto_blake2b_ctx c;
    uint8_t w = (uint8_t)who;

    crypto_blake2b_keyed_init(&c, PAKE_TAG_LEN, k, 32U);
    crypto_blake2b_update(&c, (const uint8_t *)"camex cpace tag", 15U);
    crypto_blake2b_update(&c, &w, 1U);
    crypto_blake2b_update(&c, sid, PAKE_SID_LEN);
    crypto_blake2b_update(&c, ya, 32U);
    crypto_blake2b_update(&c, yb, 32U);
    crypto_blake2b_update(&c, server_pub, 32U);
    crypto_blake2b_final(&c, out);
}

void pake_psk(uint8_t psk[32], const uint8_t k[32])
{
    crypto_blake2b_keyed(psk, 32U, k, 32U, (const uint8_t *)"camex cpace psk",
                         15U);
}
