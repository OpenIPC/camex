/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * tcpgate.c — who may take a new TCP connection that comes in through the
 * tunnel (#187); see tcpgate.h
 */
#include "tcpgate.h"

#include <string.h>

static tcpgate_fn gate = NULL;

void tcpgate_set(tcpgate_fn fn)
{
    gate = fn;
}

int tcpgate_active(void)
{
    return gate != NULL;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(((unsigned)p[0] << 8) | p[1]);
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* The ones' complement sum of 16-bit words, folded */
static uint32_t sum16(uint32_t sum, const uint8_t *p, size_t len)
{
    size_t i;

    for (i = 0; i + 1 < len; i += 2) {
        sum += rd16(p + i);
    }
    if (len & 1U) {
        sum += (uint32_t)p[len - 1] << 8;
    }
    return sum;
}

static uint16_t fold(uint32_t sum)
{
    while (sum >> 16) {
        sum = (sum & 0xffffU) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

/* RST|ACK for a SYN: from the SYN's destination back to its source, the SYN
 * (and any data it carried) acknowledged */
static void build_rst(const uint8_t *ip, size_t ihl, size_t total, uint8_t *out)
{
    const uint8_t *tcp = ip + ihl;
    size_t doff = (size_t)(tcp[12] >> 4) * 4U;
    uint32_t seq = rd32(tcp + 4);
    uint32_t data = (uint32_t)(total - ihl - doff);
    uint8_t *t = out + 20;
    uint32_t sum;

    memset(out, 0, TCPGATE_RST_LEN);
    out[0] = 0x45;
    wr16(out + 2, (uint16_t)TCPGATE_RST_LEN);
    wr16(out + 6, 0x4000);              /* DF */
    out[8] = 64;
    out[9] = 6;
    memcpy(out + 12, ip + 16, 4);       /* from the SYN's destination */
    memcpy(out + 16, ip + 12, 4);       /* to its source */
    wr16(out + 10, fold(sum16(0, out, 20)));

    memcpy(t, tcp + 2, 2);              /* source port: the SYN's destination */
    memcpy(t + 2, tcp, 2);
    wr32(t + 8, seq + 1U + data);
    t[12] = 5U << 4;
    t[13] = 0x14;                       /* RST, ACK */
    /* pseudo header: addresses, protocol, TCP length */
    sum = sum16(0, out + 12, 8);
    sum += 6U + 20U;
    sum = sum16(sum, t, 20);
    wr16(t + 16, fold(sum));
}

int tcpgate_check(const uint8_t *pkt, size_t len,
                  uint8_t *rst, size_t rst_cap, size_t *rst_len)
{
    size_t ihl, total;
    const uint8_t *tcp;
    uint32_t dst, src;

    if (rst_len != NULL) {
        *rst_len = 0U;
    }
    if (gate == NULL || pkt == NULL || len < 20U || (pkt[0] >> 4) != 4) {
        return 1;
    }
    ihl = (size_t)(pkt[0] & 0x0fU) * 4U;
    total = rd16(pkt + 2);
    if (ihl < 20U || total > len || total < ihl + 20U || pkt[9] != 6) {
        return 1;                       /* not a whole TCP segment: the kernel judges it */
    }
    if ((rd16(pkt + 6) & 0x1fffU) != 0U) {
        return 1;                       /* a later fragment */
    }
    tcp = pkt + ihl;
    if ((tcp[13] & 0x12U) != 0x02U || (size_t)(tcp[12] >> 4) * 4U < 20U ||
        ihl + (size_t)(tcp[12] >> 4) * 4U > total) {
        return 1;                       /* not a new connection */
    }
    memcpy(&dst, pkt + 16, 4);
    memcpy(&src, pkt + 12, 4);
    if (gate(dst, rd16(tcp + 2), src, rd16(tcp))) {
        return 1;
    }
    if (rst != NULL && rst_len != NULL && rst_cap >= TCPGATE_RST_LEN) {
        build_rst(pkt, ihl, total, rst);
        *rst_len = TCPGATE_RST_LEN;
    }
    return 0;
}
