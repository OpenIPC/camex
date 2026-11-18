/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * test_tcpgate.c — the gate for new inbound TCP connections of the Android
 * library (#187): which packets it asks about, and the RST it answers with
 */

#include "tcpgate.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
        if (!(cond)) { \
            printf("FAIL: %s\n", msg); \
            failures++; \
        } \
    } while (0)

static int asked;
static uint32_t got_dst, got_src;
static uint16_t got_dport, got_sport;
static int answer;

static int gate(uint32_t dst, uint16_t dport, uint32_t src, uint16_t sport)
{
    asked++;
    got_dst = dst;
    got_dport = dport;
    got_src = src;
    got_sport = sport;
    return answer;
}

/* The ones' complement sum over p, folded: 0xffff for a valid checksum */
static unsigned sum(unsigned s, const uint8_t *p, size_t n)
{
    size_t i;

    for (i = 0; i + 1 < n; i += 2) {
        s += ((unsigned)p[i] << 8) | p[i + 1];
    }
    while (s >> 16) {
        s = (s & 0xffffU) + (s >> 16);
    }
    return s;
}

/* 10.200.0.1:41000 -> 10.200.0.12:8085 with the given flags and data length */
static size_t segment(uint8_t *p, uint8_t flags, size_t data)
{
    size_t total = 20 + 20 + data;

    memset(p, 0, total);
    p[0] = 0x45;
    p[2] = (uint8_t)(total >> 8);
    p[3] = (uint8_t)total;
    p[6] = 0x40;
    p[8] = 64;
    p[9] = 6;
    p[12] = 10; p[13] = 200; p[14] = 0; p[15] = 1;
    p[16] = 10; p[17] = 200; p[18] = 0; p[19] = 12;
    p[20] = 41000 >> 8; p[21] = 41000 & 0xff;
    p[22] = 8085 >> 8;  p[23] = 8085 & 0xff;
    p[24] = 0x12; p[25] = 0x34; p[26] = 0x56; p[27] = 0x78;   /* seq */
    p[32] = 5 << 4;
    p[33] = flags;
    return total;
}

int main(void)
{
    uint8_t pkt[100], rst[TCPGATE_RST_LEN];
    size_t len, rst_len = 99;
    unsigned s;

    /* No gate: everything passes and nobody is asked */
    tcpgate_set(NULL);
    len = segment(pkt, 0x02, 0);
    CHECK(!tcpgate_active(), "no gate at start");
    CHECK(tcpgate_check(pkt, len, rst, sizeof(rst), &rst_len) == 1, "no gate: a SYN passes");
    CHECK(rst_len == 0, "no gate: no RST");

    tcpgate_set(gate);
    CHECK(tcpgate_active(), "a gate set");

    /* A SYN the gate accepts */
    asked = 0;
    answer = 1;
    CHECK(tcpgate_check(pkt, len, rst, sizeof(rst), &rst_len) == 1, "accepted SYN passes");
    CHECK(asked == 1, "the gate is asked about a SYN");
    CHECK(got_dport == 8085 && got_sport == 41000, "ports in host order");
    CHECK(memcmp(&got_dst, pkt + 16, 4) == 0 && memcmp(&got_src, pkt + 12, 4) == 0,
          "addresses as in the packet");

    /* Not new connections: the gate is not asked */
    asked = 0;
    answer = 0;
    len = segment(pkt, 0x12, 0);        /* SYN-ACK: an answer to our connect */
    CHECK(tcpgate_check(pkt, len, rst, sizeof(rst), &rst_len) == 1, "SYN-ACK passes");
    len = segment(pkt, 0x10, 10);       /* data of an open connection */
    CHECK(tcpgate_check(pkt, len, rst, sizeof(rst), &rst_len) == 1, "ACK passes");
    len = segment(pkt, 0x02, 0);
    pkt[9] = 17;                        /* UDP */
    CHECK(tcpgate_check(pkt, len, rst, sizeof(rst), &rst_len) == 1, "UDP passes");
    pkt[6] = 0x20; pkt[7] = 0x10;       /* a later fragment of UDP */
    CHECK(tcpgate_check(pkt, len, rst, sizeof(rst), &rst_len) == 1, "a UDP fragment passes");
    pkt[9] = 6;
    pkt[0] = 0x60;                      /* IPv6 */
    CHECK(tcpgate_check(pkt, len, rst, sizeof(rst), &rst_len) == 1, "IPv6 passes");
    len = segment(pkt, 0x02, 0);
    CHECK(tcpgate_check(pkt, 30, rst, sizeof(rst), &rst_len) == 1,
          "a packet shorter than its IP length passes (the kernel drops it)");
    CHECK(asked == 0, "the gate is not asked about those");

    /* TCP in fragments: dropped without asking, even when the gate would accept
     * (the kernel would put together a SYN the gate never saw whole) */
    answer = 1;
    len = segment(pkt, 0x02, 0);
    pkt[6] = 0x20; pkt[7] = 0x00;       /* a first fragment: more fragments */
    CHECK(tcpgate_check(pkt, len, rst, sizeof(rst), &rst_len) == 0 && rst_len == 0,
          "a first TCP fragment is dropped, no RST");
    pkt[6] = 0x00; pkt[7] = 0x02;       /* a later one: offset 16 bytes */
    CHECK(tcpgate_check(pkt, len, rst, sizeof(rst), &rst_len) == 0 && rst_len == 0,
          "a later TCP fragment is dropped");
    len = segment(pkt, 0x02, 0);
    pkt[3] = 28;                        /* 8 TCP bytes: the flags not even there */
    CHECK(tcpgate_check(pkt, 28, rst, sizeof(rst), &rst_len) == 0, "a cut TCP header is dropped");
    len = segment(pkt, 0x02, 0);
    pkt[32] = 15 << 4;                  /* a data offset past the end */
    CHECK(tcpgate_check(pkt, len, rst, sizeof(rst), &rst_len) == 0, "a broken data offset is dropped");
    CHECK(asked == 0, "the gate is not asked about fragments");
    answer = 0;

    /* A refused SYN with 3 bytes of data: RST|ACK back */
    len = segment(pkt, 0x02, 3);
    CHECK(tcpgate_check(pkt, len, rst, sizeof(rst), &rst_len) == 0, "refused SYN stops");
    CHECK(rst_len == TCPGATE_RST_LEN, "an RST is built");
    CHECK(rst[0] == 0x45 && rst[3] == 40 && rst[9] == 6, "RST: IPv4, 40 bytes, TCP");
    CHECK(memcmp(rst + 12, pkt + 16, 4) == 0 && memcmp(rst + 16, pkt + 12, 4) == 0,
          "RST: addresses swapped");
    CHECK(memcmp(rst + 20, pkt + 22, 2) == 0 && memcmp(rst + 22, pkt + 20, 2) == 0,
          "RST: ports swapped");
    CHECK(rst[28] == 0x12 && rst[29] == 0x34 && rst[30] == 0x56 && rst[31] == 0x78 + 4,
          "RST: acks the SYN and its data");
    CHECK(rst[33] == 0x14, "RST: flags RST|ACK");
    CHECK(sum(0, rst, 20) == 0xffff, "RST: IP checksum");
    s = sum(0, rst + 12, 8) + 6 + 20;
    CHECK(sum(s, rst + 20, 20) == 0xffff, "RST: TCP checksum");

    /* No room for the RST: refused all the same */
    CHECK(tcpgate_check(pkt, len, rst, 10, &rst_len) == 0 && rst_len == 0,
          "refused without room for an RST");

    tcpgate_set(NULL);
    if (failures == 0) {
        printf("test_tcpgate: OK\n");
    }
    return failures ? 1 : 0;
}
