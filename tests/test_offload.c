/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * test_offload.c — TUN offloads (offload.h): a TSO packet cut into
 *                  packets, the packets joined again, checksums
 *
 */

#include "offload.h"

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

static int folds_to_ones(uint32_t s)
{
    while (s >> 16) {
        s = (s & 0xFFFFU) + (s >> 16);
    }
    return s == 0xFFFFU;
}

/* The IP (v4) and TCP checksums of a packet verify */
static int tcp_ok(const uint8_t *p, size_t len, int v4)
{
    size_t l4 = v4 ? (size_t)(p[0] & 15U) * 4U : 40U;
    uint32_t s;

    if (v4) {
        if (((size_t)((p[2] << 8) | p[3])) != len ||
            !folds_to_ones(sum16(p, l4, 0U))) {
            return 0;
        }
        s = sum16(p + 12, 8U, 0U);
    } else {
        if (((size_t)((p[4] << 8) | p[5])) + 40U != len) {
            return 0;
        }
        s = sum16(p + 8, 32U, 0U);
    }
    s += 6U + (uint32_t)(len - l4);
    return folds_to_ones(sum16(p + l4, len - l4, s));
}

static size_t make_tso(uint8_t *p, int v4, size_t payload, uint8_t flags)
{
    size_t l4 = v4 ? 20U : 40U;
    size_t i;
    size_t len = l4 + 32U + payload;    /* TCP with 12 bytes of options */

    memset(p, 0, len);
    if (v4) {
        p[0] = 0x45;
        p[2] = (uint8_t)(len >> 8);
        p[3] = (uint8_t)len;
        p[4] = 0x12; p[5] = 0x34;
        p[6] = 0x40;
        p[8] = 64; p[9] = 6;
        memcpy(p + 12, "\x0a\x4f\x00\x01\x0a\x4f\x00\x68", 8);
    } else {
        p[0] = 0x60;
        p[4] = (uint8_t)((len - 40U) >> 8);
        p[5] = (uint8_t)(len - 40U);
        p[6] = 6; p[7] = 64;
        p[8] = 0xfd; p[23] = 1;
        p[24] = 0xfd; p[39] = 2;
    }
    p[l4 + 0] = 0x1F; p[l4 + 1] = 0x92;
    p[l4 + 2] = 0xC3; p[l4 + 3] = 0x50;
    p[l4 + 4] = 0xFF; p[l4 + 5] = 0xFF; p[l4 + 6] = 0xF0; p[l4 + 7] = 0x00;
    p[l4 + 8] = 0; p[l4 + 9] = 0; p[l4 + 10] = 0x10; p[l4 + 11] = 0x01;
    p[l4 + 12] = 0x80;                   /* 32 bytes of TCP header */
    p[l4 + 13] = flags;
    p[l4 + 14] = 0x01; p[l4 + 15] = 0xF5;
    memcpy(p + l4 + 20, "\x01\x01\x08\x0a\x00\x00\x00\x07\x00\x00\x00\x09",
           12U);
    for (i = 0U; i < payload; ++i) {
        p[l4 + 32U + i] = (uint8_t)(i * 7U + (i >> 8));
    }
    return len;
}

static void round_trip(int v4, size_t payload, size_t mss)
{
    static uint8_t tso[OFFLOAD_MAX];
    static uint8_t seg[64][2000];
    static size_t seg_len[64];
    static offload_gro_t g;
    size_t len = make_tso(tso, v4, payload, 0x18);  /* ACK PSH */
    size_t l4 = v4 ? 20U : 40U;
    size_t n = 0U;
    offload_seg_t s;
    ssize_t r;
    size_t i;

    CHECK(offload_seg_init(&s, tso, len, mss) == 0, "seg_init v%d",
          v4 ? 4 : 6);
    while ((r = offload_seg_next(&s, seg[n], sizeof(seg[n]))) > 0) {
        seg_len[n++] = (size_t)r;
        if (n == 64U) {
            break;
        }
    }
    CHECK(n == (payload + mss - 1U) / mss, "v%d: %u packets, not %u",
          v4 ? 4 : 6, (unsigned)n, (unsigned)((payload + mss - 1U) / mss));
    for (i = 0U; i < n; ++i) {
        const uint8_t *t = seg[i] + l4;
        int last = i + 1U == n;

        CHECK(tcp_ok(seg[i], seg_len[i], v4), "v%d packet %u: checksums",
              v4 ? 4 : 6, (unsigned)i);
        CHECK(seg_len[i] <= l4 + 32U + mss, "packet %u too long",
              (unsigned)i);
        CHECK(((t[13] & 0x08U) != 0U) == last, "PSH on the last only");
        if (v4) {
            CHECK(((seg[i][4] << 8) | seg[i][5]) == 0x1234 + (int)i,
                  "IPv4 ID of packet %u", (unsigned)i);
        }
    }
    /* The sequence numbers wrap past 2^32 here: joined all the same */
    memset(&g, 0, sizeof(g));
    for (i = 0U; i < n; ++i) {
        CHECK(offload_gro_add(&g, seg[i], seg_len[i]) == 1,
              "v%d: packet %u did not join", v4 ? 4 : 6, (unsigned)i);
    }
    CHECK(g.closed, "a short or PSH last packet closes the batch");
    {
        offload_vnet_t vh;
        const uint8_t *pkt;
        uint8_t copy[OFFLOAD_MAX];
        size_t jl = offload_gro_take(&g, &vh, &pkt);

        CHECK(jl == len, "joined %u bytes, not %u", (unsigned)jl,
              (unsigned)len);
        CHECK(n == 1U ? vh.gso_type == 0U && vh.flags == 0U :
              vh.gso_type == (v4 ? OFFLOAD_GSO_TCPV4 : OFFLOAD_GSO_TCPV6) &&
                  vh.gso_size == mss && vh.csum_start == l4 &&
                  vh.csum_offset == 16U && vh.hdr_len == l4 + 32U &&
                  (vh.flags & OFFLOAD_F_NEEDS_CSUM) != 0U,
              "v%d: the vnet header", v4 ? 4 : 6);
        memcpy(copy, pkt, jl);
        CHECK(memcmp(copy + l4 + 32U, tso + l4 + 32U, payload) == 0,
              "joined data");
        CHECK((copy[l4 + 13] & 0x08U) != 0U, "PSH kept on the joined packet");
        /* What the kernel does with NEEDS_CSUM gives a valid packet */
        CHECK((n == 1U || offload_complete_csum(copy, jl, vh.csum_start,
                                                vh.csum_offset) == 0) &&
                  tcp_ok(copy, jl, v4),
              "v%d: the joined packet's checksum", v4 ? 4 : 6);
        CHECK(g.len == 0U, "taking empties the batch");
    }
}

int main(void)
{
    static uint8_t a[2000], b[2000];
    static offload_gro_t g;
    offload_vnet_t vh;
    const uint8_t *pkt;
    size_t la, lb;

    round_trip(1, 30000U, 1448U);
    round_trip(0, 30000U, 1428U);
    round_trip(1, 5000U, 1000U);       /* the last packet of a size */
    round_trip(1, 100U, 1448U);        /* one packet */

    /* A single packet goes as it is (no GSO, its own checksum) */
    memset(&g, 0, sizeof(g));
    la = make_tso(a, 1, 1000U, 0x10);
    CHECK(offload_gro_add(&g, a, la) == 1, "a data packet starts a batch");
    CHECK(offload_gro_take(&g, &vh, &pkt) == la && vh.gso_type == 0U &&
              vh.flags == 0U, "one packet: no vnet offload");

    /* Not to join: SYN, an ACK alone, IPv4 options, a fragment */
    la = make_tso(a, 1, 1000U, 0x02);
    CHECK(offload_gro_add(&g, a, la) == -1, "SYN joined");
    la = make_tso(a, 1, 0U, 0x10);
    CHECK(offload_gro_add(&g, a, la) == -1, "a bare ACK joined");
    la = make_tso(a, 1, 1000U, 0x10);
    a[7] = 1;
    CHECK(offload_gro_add(&g, a, la) == -1, "a fragment joined");

    /* Another connection, a gap in the sequence, another ACK number */
    memset(&g, 0, sizeof(g));
    la = make_tso(a, 1, 1000U, 0x10);
    lb = make_tso(b, 1, 1000U, 0x10);
    b[20 + 1] ^= 1U;                                  /* source port */
    CHECK(offload_gro_add(&g, a, la) == 1 && offload_gro_add(&g, b, lb) == 0,
          "another connection joined");
    CHECK(!offload_gro_same_flow(&g, b, lb) && offload_gro_same_flow(&g, a, la),
          "same_flow");
    lb = make_tso(b, 1, 1000U, 0x10);                 /* same seq: a gap */
    CHECK(offload_gro_add(&g, b, lb) == 0, "out of order joined");
    lb = make_tso(b, 1, 1000U, 0x10);
    b[24] = 0xFF; b[25] = 0xFF; b[26] = 0xF3; b[27] = 0xE8;   /* next seq */
    b[30] ^= 1U;                                      /* ACK number */
    CHECK(offload_gro_add(&g, b, lb) == 0, "another ACK number joined");
    b[30] ^= 1U;
    CHECK(offload_gro_add(&g, b, lb) == 1, "the next packet did not join");

    /* UDP: a zero checksum goes as all ones */
    {
        uint8_t u[28];

        memset(u, 0, sizeof(u));
        u[0] = 0x45;
        u[20] = 0xFF;   /* sums to all ones: the checksum would be 0 */
        u[21] = 0xFF;
        CHECK(offload_complete_csum(u, sizeof(u), 20U, 6U) == 0 &&
                  u[26] == 0xFF && u[27] == 0xFF, "UDP zero checksum");
    }

    if (failures != 0) {
        fprintf(stderr, "test_offload: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_offload: OK\n");
    return 0;
}
