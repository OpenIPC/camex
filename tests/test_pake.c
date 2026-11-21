/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * test_pake.c — CPace (pake.h): both sides get one key with the same
 *               name, different keys with different names, and the tags
 *               tell them apart
 *
 */

#include "pake.h"
#include "netw.h"
#include "monocypher.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL: " __VA_ARGS__); \
            fputc('\n', stderr); \
            ++failures; \
        } \
    } while (0)

int main(void)
{
    uint8_t id[32], other[32], sid[PAKE_SID_LEN], pub[32];
    uint8_t a[32], b[32], g[32], gb[32], ya[32], yb[32], ka[32], kb[32];
    uint8_t t1[PAKE_TAG_LEN], t2[PAKE_TAG_LEN], p1[32], p2[32];
    static const uint8_t zero[32];

    netw_id_from_name("field cameras", id);
    netw_id_from_name("field camera", other);
    memset(sid, 0x11, sizeof(sid));
    memset(pub, 0x22, sizeof(pub));
    memset(a, 0x33, sizeof(a));
    memset(b, 0x44, sizeof(b));

    pake_generator(id, sid, g);
    CHECK(pake_mult(ya, a, g) == 0 && pake_mult(yb, b, g) == 0, "mult");
    CHECK(pake_mult(ka, a, yb) == 0 && pake_mult(kb, b, ya) == 0, "shared");
    CHECK(memcmp(ka, kb, 32) == 0, "the same name gives one key");
    pake_tag(t1, 's', ka, sid, ya, yb, pub);
    pake_tag(t2, 's', kb, sid, ya, yb, pub);
    CHECK(crypto_verify16(t1, t2) == 0, "server tags match");
    pake_tag(t2, 'c', kb, sid, ya, yb, pub);
    CHECK(crypto_verify16(t1, t2) != 0, "client and server tags differ");
    pake_psk(p1, ka);
    pake_psk(p2, kb);
    CHECK(memcmp(p1, p2, 32) == 0 && memcmp(p1, ka, 32) != 0, "psk");

    /* Another name: the server answers on its generator, keys differ */
    pake_generator(other, sid, gb);
    CHECK(memcmp(g, gb, 32) != 0, "two names, one generator");
    CHECK(pake_mult(yb, b, gb) == 0 && pake_mult(ka, a, yb) == 0 &&
              pake_mult(kb, b, ya) == 0, "mult (other)");
    CHECK(memcmp(ka, kb, 32) != 0, "another name gives the same key");
    /* Another session: another generator */
    sid[0] ^= 1U;
    pake_generator(id, sid, gb);
    CHECK(memcmp(g, gb, 32) != 0, "two sessions, one generator");
    /* A low-order point is refused */
    CHECK(pake_mult(ka, a, zero) != 0, "the zero point");

    if (failures != 0) {
        fprintf(stderr, "test_pake: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_pake: OK\n");
    return 0;
}
