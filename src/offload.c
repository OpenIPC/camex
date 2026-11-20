/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * offload.c — TUN offloads: TSO packets cut into the tunnel's packets,
 *             checksums completed, and TCP packets of one connection
 *             joined into one write (offload.h)
 *
 */

#include "offload.h"

#include <string.h>

#define TCP_FIN 0x01U
#define TCP_PSH 0x08U
#define TCP_ACK 0x10U
#define TCP_CWR 0x80U

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void wr16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* One's complement sum of 16-bit big-endian words, not folded */
static uint32_t sum16(const uint8_t *p, size_t len, uint32_t s)
{
    while (len > 1U) {
        s += (uint32_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2U;
        if (s & 0x80000000U) {
            s = (s & 0xFFFFU) + (s >> 16);
        }
    }
    if (len != 0U) {
        s += (uint32_t)(p[0] << 8);
    }
    return s;
}

static uint16_t fold(uint32_t s)
{
    while (s >> 16) {
        s = (s & 0xFFFFU) + (s >> 16);
    }
    return (uint16_t)s;
}

void offload_vnet_parse(offload_vnet_t *vh, const uint8_t in[OFFLOAD_VNET_LEN])
{
    vh->flags = in[0];
    vh->gso_type = in[1];
    memcpy(&vh->hdr_len, in + 2, 2);
    memcpy(&vh->gso_size, in + 4, 2);
    memcpy(&vh->csum_start, in + 6, 2);
    memcpy(&vh->csum_offset, in + 8, 2);
}

void offload_vnet_write(uint8_t out[OFFLOAD_VNET_LEN], const offload_vnet_t *vh)
{
    out[0] = vh->flags;
    out[1] = vh->gso_type;
    memcpy(out + 2, &vh->hdr_len, 2);
    memcpy(out + 4, &vh->gso_size, 2);
    memcpy(out + 6, &vh->csum_start, 2);
    memcpy(out + 8, &vh->csum_offset, 2);
}

int offload_complete_csum(uint8_t *pkt, size_t len, size_t start, size_t off)
{
    uint16_t c;

    if (start >= len || off + 2U > len - start) {
        return -1;
    }
    c = (uint16_t)~fold(sum16(pkt + start, len - start, 0U));
    /* UDP: a zero sum is sent as all ones (zero means "none") */
    if (c == 0U && off == 6U) {
        c = 0xFFFFU;
    }
    wr16(pkt + start + off, c);
    return 0;
}

/* The pseudo-header sum of a TCP packet of l4len bytes */
static uint32_t pseudo(const uint8_t *pkt, int v4, size_t l4len)
{
    uint32_t s;

    if (v4) {
        s = sum16(pkt + 12, 8U, 0U);
    } else {
        s = sum16(pkt + 8, 32U, 0U);
    }
    return s + 6U + (uint32_t)(l4len & 0xFFFFU) + (uint32_t)(l4len >> 16);
}

static void ipv4_csum(uint8_t *pkt)
{
    size_t ihl = (size_t)(pkt[0] & 0x0FU) * 4U;

    pkt[10] = pkt[11] = 0U;
    wr16(pkt + 10, (uint16_t)~fold(sum16(pkt, ihl, 0U)));
}

/* Where TCP starts and where its data starts; -1 when pkt is not a whole
 * IPv4/IPv6 TCP packet (fragments and IPv6 extension headers included) */
static int tcp_offsets(const uint8_t *pkt, size_t len, int *v4, size_t *l4,
                       size_t *hdrs)
{
    if (len < 20U) {
        return -1;
    }
    if ((pkt[0] >> 4) == 4U) {
        size_t ihl = (size_t)(pkt[0] & 0x0FU) * 4U;

        if (ihl < 20U || pkt[9] != 6U || rd16(pkt + 2) != len ||
            (rd16(pkt + 6) & 0x3FFFU) != 0U) {
            return -1;
        }
        *v4 = 1;
        *l4 = ihl;
    } else if ((pkt[0] >> 4) == 6U) {
        if (len < 40U || pkt[6] != 6U || (size_t)rd16(pkt + 4) + 40U != len) {
            return -1;
        }
        *v4 = 0;
        *l4 = 40U;
    } else {
        return -1;
    }
    if (len < *l4 + 20U) {
        return -1;
    }
    *hdrs = *l4 + (size_t)(pkt[*l4 + 12U] >> 4) * 4U;
    if (*hdrs < *l4 + 20U || *hdrs > len) {
        return -1;
    }
    return 0;
}

int offload_seg_init(offload_seg_t *s, const uint8_t *pkt, size_t len,
                     size_t mss)
{
    memset(s, 0, sizeof(*s));
    if (mss == 0U || len > OFFLOAD_MAX) {
        return -1;
    }
    if (len < 40U || ((pkt[0] >> 4) != 4U && (pkt[0] >> 4) != 6U)) {
        return -1;
    }
    s->pkt = pkt;
    s->len = len;
    s->mss = mss;
    if ((pkt[0] >> 4) == 4U) {
        size_t ihl = (size_t)(pkt[0] & 0x0FU) * 4U;

        if (ihl < 20U || pkt[9] != 6U || len < ihl + 20U) {
            return -1;
        }
        s->v4 = 1;
        s->l4 = ihl;
        s->id = rd16(pkt + 4);
    } else {
        if (pkt[6] != 6U || len < 60U) {
            return -1;
        }
        s->l4 = 40U;
    }
    s->hdrs = s->l4 + (size_t)(pkt[s->l4 + 12U] >> 4) * 4U;
    if (s->hdrs < s->l4 + 20U || s->hdrs > len) {
        return -1;
    }
    s->seq = rd32(pkt + s->l4 + 4U);
    s->active = 1;
    return 0;
}

ssize_t offload_seg_next(offload_seg_t *s, uint8_t *out, size_t size)
{
    size_t payload;
    size_t n;
    size_t total;
    size_t index;
    uint8_t *tcp;
    int last;

    if (!s->active) {
        return 0;
    }
    payload = s->len - s->hdrs;
    n = payload - s->off < s->mss ? payload - s->off : s->mss;
    total = s->hdrs + n;
    if (total > size) {
        s->active = 0;
        return -1;
    }
    index = s->off / s->mss;
    last = s->off + n >= payload;
    memcpy(out, s->pkt, s->hdrs);
    memcpy(out + s->hdrs, s->pkt + s->hdrs + s->off, n);
    tcp = out + s->l4;
    if (s->v4) {
        wr16(out + 2, (uint32_t)total);
        wr16(out + 4, (uint32_t)(uint16_t)(s->id + index));
        ipv4_csum(out);
    } else {
        wr16(out + 4, (uint32_t)(total - 40U));
    }
    {
        uint32_t seq = s->seq + (uint32_t)s->off;

        tcp[4] = (uint8_t)(seq >> 24);
        tcp[5] = (uint8_t)(seq >> 16);
        tcp[6] = (uint8_t)(seq >> 8);
        tcp[7] = (uint8_t)seq;
    }
    if (!last) {
        tcp[13] &= (uint8_t)~(TCP_FIN | TCP_PSH);   /* the last only */
    }
    if (index != 0U) {
        tcp[13] &= (uint8_t)~TCP_CWR;               /* the first only */
    }
    tcp[16] = tcp[17] = 0U;
    wr16(tcp + 16, (uint16_t)~fold(sum16(tcp, total - s->l4,
                                         pseudo(out, s->v4,
                                                total - s->l4))));
    s->off += n;
    if (last) {
        s->active = 0;
    }
    return (ssize_t)total;
}

int offload_gro_same_flow(const offload_gro_t *g, const uint8_t *pkt,
                          size_t len)
{
    if (g->len == 0U || len < g->l4 + 4U || ((pkt[0] >> 4) == 4U) != g->v4) {
        return 0;
    }
    if (g->v4) {
        return len >= 20U && pkt[9] == g->buf[9] &&
               (size_t)(pkt[0] & 0x0FU) * 4U == g->l4 &&
               memcmp(pkt + 12, g->buf + 12, 8U) == 0 &&
               memcmp(pkt + g->l4, g->buf + g->l4, 4U) == 0;
    }
    return len >= 44U && pkt[6] == g->buf[6] &&
           memcmp(pkt + 8, g->buf + 8, 32U) == 0 &&
           memcmp(pkt + 40, g->buf + 40, 4U) == 0;
}

int offload_gro_add(offload_gro_t *g, const uint8_t *pkt, size_t len)
{
    int v4;
    size_t l4;
    size_t hdrs;
    size_t payload;
    const uint8_t *tcp;
    uint8_t flags;

    if (tcp_offsets(pkt, len, &v4, &l4, &hdrs) != 0) {
        return -1;
    }
    tcp = pkt + l4;
    flags = tcp[13];
    payload = len - hdrs;
    /* Data with ACK (and PSH) alone; IPv4 without options */
    if (payload == 0U || (flags & (uint8_t)~(TCP_ACK | TCP_PSH)) != 0U ||
        (flags & TCP_ACK) == 0U || (v4 && l4 != 20U)) {
        return -1;
    }
    if (g->len == 0U) {
        memcpy(g->buf, pkt, len);
        g->len = len;
        g->hdrs = hdrs;
        g->l4 = l4;
        g->mss = payload;
        g->count = 1U;
        g->next_seq = rd32(tcp + 4) + (uint32_t)payload;
        g->v4 = v4;
        g->closed = (flags & TCP_PSH) != 0U;
        return 1;
    }
    if (g->closed || v4 != g->v4 || hdrs != g->hdrs ||
        payload > g->mss || g->len + payload > OFFLOAD_GRO_MAX ||
        g->count >= 64U || !offload_gro_same_flow(g, pkt, len) ||
        rd32(tcp + 4) != g->next_seq) {
        return 0;
    }
    {
        const uint8_t *first = g->buf;
        const uint8_t *ftcp = first + l4;

        if (v4) {
            /* TOS, TTL and DF as the first packet's */
            if (pkt[1] != first[1] || pkt[8] != first[8] ||
                (pkt[6] & 0x40U) != (first[6] & 0x40U)) {
                return 0;
            }
        } else if (memcmp(pkt, first, 4U) != 0 || pkt[7] != first[7]) {
            return 0;   /* traffic class, flow label, hop limit */
        }
        /* The same ACK, header length, window and options; flags may
         * differ in PSH only */
        if (memcmp(tcp + 8, ftcp + 8, 4U) != 0 || tcp[12] != ftcp[12] ||
            ((tcp[13] ^ ftcp[13]) & (uint8_t)~TCP_PSH) != 0U ||
            memcmp(tcp + 14, ftcp + 14, 2U) != 0 ||
            memcmp(tcp + 20, ftcp + 20, hdrs - l4 - 20U) != 0) {
            return 0;
        }
    }
    memcpy(g->buf + g->len, pkt + hdrs, payload);
    g->len += payload;
    g->count++;
    g->next_seq += (uint32_t)payload;
    if (payload < g->mss || (flags & TCP_PSH) != 0U) {
        g->buf[g->l4 + 13U] |= (uint8_t)(flags & TCP_PSH);
        g->closed = 1;
    }
    return 1;
}

size_t offload_gro_take(offload_gro_t *g, offload_vnet_t *vh,
                        const uint8_t **pkt)
{
    size_t len = g->len;

    memset(vh, 0, sizeof(*vh));
    *pkt = g->buf;
    if (len == 0U) {
        return 0U;
    }
    if (g->count > 1U) {
        uint8_t *tcp = g->buf + g->l4;

        if (g->v4) {
            wr16(g->buf + 2, (uint32_t)len);
            ipv4_csum(g->buf);
        } else {
            wr16(g->buf + 4, (uint32_t)(len - 40U));
        }
        /* CHECKSUM_PARTIAL: the field holds the pseudo-header sum */
        wr16(tcp + 16, fold(pseudo(g->buf, g->v4, len - g->l4)));
        vh->flags = OFFLOAD_F_NEEDS_CSUM;
        vh->gso_type = g->v4 ? OFFLOAD_GSO_TCPV4 : OFFLOAD_GSO_TCPV6;
        vh->hdr_len = (uint16_t)g->hdrs;
        vh->gso_size = (uint16_t)g->mss;
        vh->csum_start = (uint16_t)g->l4;
        vh->csum_offset = 16U;
    }
    g->len = 0U;
    g->count = 0U;
    g->closed = 0;
    return len;
}
