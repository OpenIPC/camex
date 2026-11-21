/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * test_chacha_neon.c — ChaCha20 and Poly1305 with NEON give what the plain
 *                      code gives
 *                      (32-bit ARM with NEON; elsewhere it says so and
 *                      passes)
 *
 */

#include "chacha_neon.h"
#include "monocypher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    static uint8_t in[4096], got[4096], want[4096];
    uint8_t key[32], nonce[8];
    unsigned int seed = 7U;
    size_t len;
    int failures = 0;
    int round;

    if (!camex_chacha_neon_ok()) {
        printf("test_chacha_neon: OK (no NEON here: nothing to compare)\n");
        return 0;
    }
    for (round = 0; round < 200; ++round) {
        uint64_t ctr;
        size_t i;

        for (i = 0U; i < sizeof(key); ++i) {
            key[i] = (uint8_t)(seed = seed * 1103515245U + 12345U);
        }
        for (i = 0U; i < sizeof(nonce); ++i) {
            nonce[i] = (uint8_t)(seed = seed * 1103515245U + 12345U);
        }
        for (i = 0U; i < sizeof(in); ++i) {
            in[i] = (uint8_t)(i * 31U + (unsigned)round);
        }
        len = (size_t)(round * 37) % 2048U + 1U;
        /* Counters near the 32-bit wrap too */
        ctr = (round % 5 == 0) ? 0xFFFFFFFDULL : (uint64_t)round;
        crypto_chacha20_djb(got, (round & 1) ? NULL : in, len, key, nonce,
                            ctr);
        /* The reference: one block at a time never takes the NEON path */
        for (i = 0U; i < len; i += 64U) {
            size_t n = len - i < 64U ? len - i : 64U;

            crypto_chacha20_djb(want + i, (round & 1) ? NULL : in + i, n,
                                key, nonce, ctr + i / 64U);
        }
        if (memcmp(got, want, len) != 0) {
            fprintf(stderr, "FAIL: round %d, %u bytes, counter %llx\n", round,
                    (unsigned)len, (unsigned long long)ctr);
            ++failures;
        }
    }
    /* Poly1305: the whole message at once (NEON from four blocks on)
     * against 16 bytes at a time (the plain code); keys and messages of
     * all ones too, for the largest limbs and carries */
    for (round = 0; round < 600; ++round) {
        uint8_t pkey[32], mac1[16], mac2[16];
        crypto_poly1305_ctx ctx;
        size_t i;

        for (i = 0U; i < sizeof(pkey); ++i) {
            pkey[i] = (round % 7 == 0) ? 0xFFU
                      : (uint8_t)(seed = seed * 1103515245U + 12345U);
        }
        len = (size_t)(round * 53) % 3000U;
        for (i = 0U; i < len; ++i) {
            in[i] = (round % 3 == 0) ? 0xFFU
                    : (uint8_t)(seed = seed * 1103515245U + 12345U);
        }
        crypto_poly1305(mac1, in, len, pkey);
        crypto_poly1305_init(&ctx, pkey);
        /* An odd split first: the NEON part then starts mid-message */
        crypto_poly1305_update(&ctx, in, len < 5U ? len : 5U);
        for (i = len < 5U ? len : 5U; i < len; i += 16U) {
            crypto_poly1305_update(&ctx, in + i, len - i < 16U ? len - i : 16U);
        }
        crypto_poly1305_final(&ctx, mac2);
        if (memcmp(mac1, mac2, 16) != 0) {
            fprintf(stderr, "FAIL: poly1305 round %d, %u bytes\n", round,
                    (unsigned)len);
            ++failures;
        }
    }
    /* A known answer (RFC 8439 2.5.2): the plain code alone is too short */
    {
        static const uint8_t k[32] = {
            0x85, 0xd6, 0xbe, 0x78, 0x57, 0x55, 0x6d, 0x33, 0x7f, 0x44, 0x52,
            0xfe, 0x42, 0xd5, 0x06, 0xa8, 0x01, 0x03, 0x80, 0x8a, 0xfb, 0x0d,
            0xb2, 0xfd, 0x4a, 0xbf, 0xf6, 0xaf, 0x41, 0x49, 0xf5, 0x1b };
        static const uint8_t want_tag[16] = {
            0xa8, 0x06, 0x1d, 0xc1, 0x30, 0x51, 0x36, 0xc6, 0xc2, 0x2b, 0x8b,
            0xaf, 0x0c, 0x01, 0x27, 0xa9 };
        static const char msg[] = "Cryptographic Forum Research Group";
        uint8_t mac[16];

        crypto_poly1305(mac, (const uint8_t *)msg, sizeof(msg) - 1U, k);
        if (memcmp(mac, want_tag, 16) != 0) {
            fprintf(stderr, "FAIL: poly1305 RFC 8439 vector\n");
            ++failures;
        }
    }
    if (failures != 0) {
        fprintf(stderr, "test_chacha_neon: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_chacha_neon: OK (NEON)\n");
    return 0;
}
