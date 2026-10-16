/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * test_session.c — protocol v2 transport sessions and replay window (#17)
 *
 */

#include "session.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

int main(void)
{
    replay_window_t r;
    session_t a, b;
    uint8_t k1[NOISE_KEYLEN], k2[NOISE_KEYLEN];
    uint8_t pkt[256], pkt2[256], plain[256];
    uint8_t t1[V2_TIMESTAMP_LEN], t2[V2_TIMESTAMP_LEN];
    size_t len, len2, plen = 0U;
    uint64_t n;

    /* Replay window */
    memset(&r, 0, sizeof(r));
    check(replay_check(&r, 5U) == 0, "first counter accepted");
    replay_record(&r, 5U);
    check(replay_check(&r, 5U) != 0, "repeated counter rejected");
    check(replay_check(&r, 3U) == 0, "older unseen counter accepted");
    replay_record(&r, 3U);
    check(replay_check(&r, 3U) != 0, "older counter recorded");
    replay_record(&r, 5000U);
    check(replay_check(&r, 3U) != 0, "counter below the window rejected");
    check(replay_check(&r, 5000U - 1900U) == 0, "counter inside the window");
    check(replay_check(&r, 5000U - 2100U) != 0, "counter 2100 back rejected");
    for (n = 5001U; n < 5200U; ++n) {
        replay_record(&r, n);
    }
    check(replay_check(&r, 5100U) != 0 && replay_check(&r, 5200U) == 0,
          "window slides");
    check(replay_check(&r, V2_REJECT_AFTER_MSGS) != 0, "counter limit");

    /* Sessions: a sends to b */
    memset(k1, 1, sizeof(k1));
    memset(k2, 2, sizeof(k2));
    session_init(&a, 0x11111111U, 0x22222222U, k1, k2, 0);
    session_init(&b, 0x22222222U, 0x11111111U, k2, k1, 0);
    len = session_seal(&a, (const uint8_t *)"hello", 5U, pkt, sizeof(pkt));
    check(len == V2_OVERHEAD + 5U && v2_packet_type(pkt, len) == V2_TYPE_DATA &&
          v2_data_receiver(pkt) == 0x22222222U, "DATA packet built");
    check(session_open(&b, pkt, len, plain, sizeof(plain), &plen) == 0 &&
          plen == 5U && memcmp(plain, "hello", 5) == 0, "DATA opened");
    check(session_open(&b, pkt, len, plain, sizeof(plain), &plen) != 0,
          "replayed DATA rejected");
    len2 = session_seal(&a, NULL, 0U, pkt2, sizeof(pkt2));
    check(len2 == V2_OVERHEAD &&
          session_open(&b, pkt2, len2, plain, sizeof(plain), &plen) == 0 &&
          plen == 0U, "keepalive (empty DATA)");
    len = session_seal(&a, (const uint8_t *)"x", 1U, pkt, sizeof(pkt));
    pkt[9] ^= 0x01U;   /* counter is authenticated (AD) */
    check(session_open(&b, pkt, len, plain, sizeof(plain), &plen) != 0,
          "tampered header rejected");
    pkt[9] ^= 0x01U;
    check(session_open(&a, pkt, len, plain, sizeof(plain), &plen) != 0,
          "packet for another index rejected");
    check(session_open(&b, pkt, len, plain, sizeof(plain), &plen) == 0,
          "untampered packet still opens");
    a.send_n = V2_REJECT_AFTER_MSGS;
    check(session_seal(&a, NULL, 0U, pkt, sizeof(pkt)) == 0U,
          "no packets after the counter limit");

    /* Timestamps increase */
    v2_timestamp(t1);
    v2_timestamp(t2);
    check(v2_timestamp_cmp(t2, t1) >= 0, "timestamps do not go back");
    check(v2_new_index() != 0U, "index is nonzero");
    check(v2_packet_type((const uint8_t *)"CX2Pxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", 30U)
          == 0U, "v1 packets are not v2");

    session_wipe(&a);
    session_wipe(&b);
    if (failures != 0) {
        fprintf(stderr, "test_session: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_session: OK\n");
    return 0;
}
