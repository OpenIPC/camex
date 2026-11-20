/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * test_chacha_neon.c — ChaCha20 with NEON gives what the plain code gives
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
    if (failures != 0) {
        fprintf(stderr, "test_chacha_neon: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_chacha_neon: OK (NEON)\n");
    return 0;
}
