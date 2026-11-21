/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * test_l2.c — TAP networks: frames in pieces and back, MACs, the MAC
 *             table, ARP replies of the gateway
 *
 */

#include "l2.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
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

/* Payloads l2_send_frame() made */
static uint8_t sent[16][1U + L2_FRAG_HDR + L2_PIECE];
static size_t sent_len[16];
static size_t sent_n;

static int collect(void *ctx, const uint8_t *payload, size_t len)
{
    (void)ctx;
    if (sent_n >= 16U || len > sizeof(sent[0])) {
        return -1;
    }
    memcpy(sent[sent_n], payload, len);
    sent_len[sent_n++] = len;
    return 0;
}

static void round_trip(size_t len, int reverse)
{
    static uint8_t frame[L2_FRAME_MAX];
    static l2_reasm_t r;
    const uint8_t *out = NULL;
    long got = 0;
    size_t i;

    for (i = 0U; i < len; ++i) {
        frame[i] = (uint8_t)(i * 7U + len);
    }
    sent_n = 0U;
    CHECK(l2_send_frame(frame, len, collect, NULL) == 0, "send %u",
          (unsigned)len);
    for (i = 0U; i < sent_n; ++i) {
        size_t k = reverse ? sent_n - 1U - i : i;

        CHECK(sent_len[k] <= 1U + L2_FRAG_HDR + L2_PIECE,
              "a payload of %u bytes", (unsigned)sent_len[k]);
        got = l2_receive(&r, sent[k], sent_len[k], &out, 100);
        if (i + 1U < sent_n) {
            CHECK(got == 0, "frame of %u complete after piece %u",
                  (unsigned)len, (unsigned)i);
        }
    }
    CHECK(got == (long)len && out != NULL && memcmp(out, frame, len) == 0,
          "frame of %u bytes (%s) came back as %ld", (unsigned)len,
          reverse ? "pieces reversed" : "in order", got);
}

int main(void)
{
    static const size_t sizes[] = { 60, 1304, 1305, 1306, 1514, 2600, 2601,
                                    9014 };
    size_t i;

    for (i = 0U; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        round_trip(sizes[i], 0);
        round_trip(sizes[i], 1);
    }
    sent_n = 0U;
    l2_send_frame((const uint8_t *)"0123456789abcdef", 16U, collect, NULL);
    CHECK(sent_n == 1U && sent[0][0] == L2_WHOLE, "a small frame is whole");
    {
        static uint8_t big[L2_FRAME_MAX + 1U];

        CHECK(l2_send_frame(big, sizeof(big), collect, NULL) != 0,
              "a frame over L2_FRAME_MAX");
    }

    /* Malformed payloads */
    {
        static l2_reasm_t r;
        const uint8_t *out;
        uint8_t bad[1U + L2_FRAG_HDR + L2_PIECE];

        memset(bad, 0, sizeof(bad));
        bad[0] = 0x07U;
        CHECK(l2_receive(&r, bad, 20U, &out, 1) < 0, "an unknown type");
        bad[0] = L2_WHOLE;
        CHECK(l2_receive(&r, bad, 5U, &out, 1) < 0, "a frame of 4 bytes");
        bad[0] = L2_FRAG;
        bad[3] = 3U;   /* index 3 of 2 */
        bad[4] = 2U;
        CHECK(l2_receive(&r, bad, 100U, &out, 1) < 0, "an index past count");
        bad[3] = 0U;
        bad[4] = 2U;   /* a first piece that is short */
        CHECK(l2_receive(&r, bad, 100U, &out, 1) < 0, "a short first piece");
        bad[4] = 9U;
        CHECK(l2_receive(&r, bad, L2_FRAG_HDR + L2_PIECE, &out, 1) < 0,
              "more than L2_MAX_PIECES");
        CHECK(l2_receive(&r, bad, L2_FRAG_HDR + L2_PIECE + 1U, &out, 1) < 0,
              "a piece longer than L2_PIECE");
        bad[4] = 2U;
        CHECK(l2_receive(NULL, bad, L2_FRAG_HDR + L2_PIECE, &out, 1) == 0,
              "pieces without a buffer are dropped");
    }

    /* A piece of a stale frame does not complete a new one */
    {
        static uint8_t frame[2000];
        static l2_reasm_t r;
        const uint8_t *out = NULL;

        memset(frame, 0xAB, sizeof(frame));
        sent_n = 0U;
        l2_send_frame(frame, sizeof(frame), collect, NULL);
        CHECK(l2_receive(&r, sent[0], sent_len[0], &out, 10) == 0, "piece 1");
        CHECK(l2_receive(&r, sent[1], sent_len[1], &out,
                         10 + L2_REASM_TIMEOUT + 1) == 0,
              "a piece after the timeout completes a stale frame");
    }

    /* MACs */
    {
        uint8_t id[32], m1[6], m2[6], m3[6];
        char text[18];

        memset(id, 0x42, sizeof(id));
        l2_client_mac(id, "CAM-A", m1);
        l2_client_mac(id, "cam-a", m2);
        l2_gateway_mac(id, m3);
        CHECK(memcmp(m1, m2, 6) == 0, "a client MAC depends on the ID's case");
        CHECK((m1[0] & 3U) == 2U && (m3[0] & 3U) == 2U,
              "MACs are local unicast");
        CHECK(memcmp(m1, m3, 6) != 0, "the gateway has a client's MAC");
        l2_mac_text(m1, text, sizeof(text));
        CHECK(l2_mac_parse(text, m2) == 0 && memcmp(m1, m2, 6) == 0,
              "MAC text round trip %s", text);
        CHECK(l2_mac_parse("02:00:00:00:00", m2) != 0, "a short MAC");
    }

    /* The MAC table */
    {
        uint8_t mac[6] = { 0x02, 1, 2, 3, 4, 5 };
        uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

        l2_mac_clear();
        l2_mac_learn(1U, mac, 7U, 1000);
        CHECK(l2_mac_lookup(1U, mac, 1001) == 7U, "a learned MAC");
        CHECK(l2_mac_lookup(2U, mac, 1001) == 0U, "a MAC of another network");
        l2_mac_learn(1U, mac, 9U, 1002);
        CHECK(l2_mac_lookup(1U, mac, 1003) == 9U, "a MAC that moved");
        CHECK(l2_mac_lookup(1U, mac, 1003 + 301) == 0U, "an aged MAC");
        l2_mac_forget_slot(9U);
        CHECK(l2_mac_lookup(1U, mac, 1003) == 0U, "a forgotten slot");
        l2_mac_learn(1U, bcast, 3U, 1000);
        CHECK(l2_mac_lookup(1U, bcast, 1000) == 0U, "a broadcast source");
    }

    /* ARP for the gateway */
    {
        uint8_t req[42], out[64];
        uint8_t gw[6] = { 0x02, 0xCA, 0, 0, 0, 1 };
        uint32_t ip;
        uint32_t asker;

        inet_pton(AF_INET, "10.66.0.1", &ip);
        inet_pton(AF_INET, "10.66.0.77", &asker);
        memset(req, 0xFF, 6);
        memcpy(req + 6, "\x02\x11\x22\x33\x44\x55", 6);
        req[12] = 0x08; req[13] = 0x06;
        memcpy(req + 14, "\x00\x01\x08\x00\x06\x04\x00\x01", 8);
        memcpy(req + 22, req + 6, 6);
        memcpy(req + 28, &asker, 4);
        memset(req + 32, 0, 6);
        memcpy(req + 38, &ip, 4);
        CHECK(l2_arp_reply(req, sizeof(req), ip, gw, out, sizeof(out)) == 42U,
              "no ARP reply for the gateway");
        CHECK(memcmp(out, req + 6, 6) == 0 && memcmp(out + 6, gw, 6) == 0 &&
                  out[21] == 2U && memcmp(out + 22, gw, 6) == 0 &&
                  memcmp(out + 28, &ip, 4) == 0 &&
                  memcmp(out + 38, &asker, 4) == 0,
              "a wrong ARP reply");
        inet_pton(AF_INET, "10.66.0.2", &ip);
        CHECK(l2_arp_reply(req, sizeof(req), ip, gw, out, sizeof(out)) == 0U,
              "an ARP reply for another address");
    }

    if (failures != 0) {
        fprintf(stderr, "test_l2: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_l2: OK\n");
    return 0;
}
