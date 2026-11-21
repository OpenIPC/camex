/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * test_bridge.c — the bridge's offload work: checksums completed, a TSO
 *                 segment cut into frames (bridge.h)
 *
 */

#include "bridge.h"

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

static uint32_t sum16(const uint8_t *p, size_t len, uint32_t s)
{
    while (len > 1U) {
        s += (uint32_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2U;
    }
    if (len) {
        s += (uint32_t)(p[0] << 8);
    }
    return s;
}

static int folds_to_zero(uint32_t s)
{
    while (s >> 16) {
        s = (s & 0xFFFFU) + (s >> 16);
    }
    return s == 0xFFFFU;
}

/* A frame's IPv4 and TCP checksums verify */
static int v4_tcp_ok(const uint8_t *f, size_t len)
{
    const uint8_t *ip = f + 14;
    size_t ihl = (size_t)(ip[0] & 15U) * 4U;
    size_t tot = (size_t)((ip[2] << 8) | ip[3]);
    uint32_t s;

    if (tot + 14U != len || !folds_to_zero(sum16(ip, ihl, 0U))) {
        return 0;
    }
    s = sum16(ip + 12, 8U, 0U) + 6U + (uint32_t)(tot - ihl);
    return folds_to_zero(sum16(ip + ihl, tot - ihl, s));
}

static uint8_t got[64][2000];
static size_t got_len[64];
static size_t got_n;

static int collect(const uint8_t *f, size_t len)
{
    if (got_n >= 64U || len > sizeof(got[0])) {
        return -1;
    }
    memcpy(got[got_n], f, len);
    got_len[got_n++] = len;
    return 0;
}

int main(void)
{
    static uint8_t f[14 + 20 + 20 + 10000];
    size_t payload = 10000U;
    size_t len = 14U + 40U + payload;
    size_t i;
    uint32_t seq0 = 0x01020304U;

    /* A TSO segment as the kernel hands it over: lengths and checksums
     * of the whole, the TCP checksum only partial */
    memset(f, 0, sizeof(f));
    memcpy(f, "\x02\x00\x00\x00\x00\x01\x02\x00\x00\x00\x00\x02\x08\x00", 14);
    f[14] = 0x45;
    f[16] = (uint8_t)((40U + payload) >> 8);
    f[17] = (uint8_t)(40U + payload);
    f[18] = 0x12; f[19] = 0x34;          /* id */
    f[20] = 0x40;                        /* DF */
    f[22] = 64; f[23] = 6;
    memcpy(f + 26, "\xc0\xa8\x4d\x02\xc0\xa8\x4d\x87", 8);
    f[34] = 0x1F; f[35] = 0x90;          /* 8080 */
    f[36] = 0xC3; f[37] = 0x50;
    f[38] = (uint8_t)(seq0 >> 24); f[39] = (uint8_t)(seq0 >> 16);
    f[40] = (uint8_t)(seq0 >> 8);  f[41] = (uint8_t)seq0;
    f[46] = 0x50;                        /* data offset 5 */
    f[47] = 0x19;                        /* FIN PSH ACK */
    f[48] = 0xFF; f[49] = 0xFF;
    for (i = 0U; i < payload; ++i) {
        f[54 + i] = (uint8_t)(i * 13U);
    }
    got_n = 0U;
    CHECK(bridge_segment(f, len, 1448U, collect) == 0, "segment");
    CHECK(got_n == (payload + 1447U) / 1448U, "%u frames, not %u",
          (unsigned)got_n, (unsigned)((payload + 1447U) / 1448U));
    {
        size_t off = 0U;

        for (i = 0U; i < got_n; ++i) {
            const uint8_t *g = got[i];
            size_t n = got_len[i] - 54U;
            uint32_t seq = ((uint32_t)g[38] << 24) | ((uint32_t)g[39] << 16) |
                           ((uint32_t)g[40] << 8) | g[41];
            int last = i + 1U == got_n;

            CHECK(v4_tcp_ok(g, got_len[i]), "frame %u: a bad checksum",
                  (unsigned)i);
            CHECK(got_len[i] <= 14U + 1500U, "frame %u longer than the MTU",
                  (unsigned)i);
            CHECK(seq == seq0 + (uint32_t)off, "frame %u: seq", (unsigned)i);
            CHECK(memcmp(g + 54, f + 54 + off, n) == 0, "frame %u: payload",
                  (unsigned)i);
            CHECK(((g[47] & 0x09U) != 0U) == last,
                  "frame %u: FIN/PSH only on the last", (unsigned)i);
            CHECK(((g[18] << 8) | g[19]) == 0x1234 + (int)i,
                  "frame %u: IP id", (unsigned)i);
            off += n;
        }
        CHECK(off == payload, "%u payload bytes out of %u", (unsigned)off,
              (unsigned)payload);
    }

    /* Checksum offload: a frame whose checksums are left to the NIC */
    {
        uint8_t one[14 + 40 + 100];

        memcpy(one, f, 54);
        one[16] = 0; one[17] = 140;
        memset(one + 54, 0x5A, 100);
        one[24] = one[25] = 0;
        one[50] = 0xAB; one[51] = 0xCD;  /* garbage, as with offload */
        CHECK(bridge_fix_checksum(one, sizeof(one), 14U) == 0 &&
                  v4_tcp_ok(one, sizeof(one)),
              "a completed checksum does not verify");
    }

    if (failures != 0) {
        fprintf(stderr, "test_bridge: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_bridge: OK\n");
    return 0;
}
