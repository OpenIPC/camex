/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * l2.c — Ethernet (TAP) networks: frame pieces, MACs, the server's MAC
 *        table, ARP replies of the gateway
 *
 */

#include "l2.h"
#include "monocypher.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

int l2_send_frame(const uint8_t *frame, size_t len, l2_send_fn send,
                  void *ctx)
{
    static uint16_t next_id = 0U;
    uint8_t buf[1U + L2_FRAG_HDR + L2_PIECE];
    size_t count;
    size_t i;
    uint16_t id;

    if (len == 0U || len > L2_FRAME_MAX) {
        return -1;
    }
    if (len <= L2_PIECE + L2_FRAG_HDR - 1U) {
        /* One payload: the type byte and the frame */
        uint8_t whole[1U + L2_PIECE + L2_FRAG_HDR];

        whole[0] = L2_WHOLE;
        memcpy(whole + 1, frame, len);
        return send(ctx, whole, len + 1U);
    }
    count = (len + L2_PIECE - 1U) / L2_PIECE;
    id = ++next_id;
    for (i = 0U; i < count; ++i) {
        size_t off = i * L2_PIECE;
        size_t n = len - off < L2_PIECE ? len - off : L2_PIECE;

        buf[0] = L2_FRAG;
        buf[1] = (uint8_t)(id >> 8);
        buf[2] = (uint8_t)id;
        buf[3] = (uint8_t)i;
        buf[4] = (uint8_t)count;
        memcpy(buf + L2_FRAG_HDR, frame + off, n);
        if (send(ctx, buf, L2_FRAG_HDR + n) != 0) {
            return -1;
        }
    }
    return 0;
}

long l2_receive(l2_reasm_t *r, const uint8_t *payload, size_t len,
                const uint8_t **frame, time_t now)
{
    uint16_t id;
    uint8_t idx;
    uint8_t count;
    size_t n;

    if (len < 1U) {
        return -1;
    }
    if (payload[0] == L2_WHOLE) {
        if (len < 1U + L2_ETH_HDR) {
            return -1;
        }
        *frame = payload + 1;
        return (long)(len - 1U);
    }
    if (payload[0] != L2_FRAG || len <= L2_FRAG_HDR) {
        return -1;
    }
    id = (uint16_t)((payload[1] << 8) | payload[2]);
    idx = payload[3];
    count = payload[4];
    n = len - L2_FRAG_HDR;
    if (count < 2U || count > L2_MAX_PIECES || idx >= count ||
        n > L2_PIECE || (idx + 1U < count && n != L2_PIECE)) {
        return -1;
    }
    if (r == NULL) {
        return 0;
    }
    if (!r->active || r->id != id || r->count != count ||
        difftime(now, r->started) > (double)L2_REASM_TIMEOUT) {
        /* Another frame starts: the one in assembly is given up */
        r->active = 1U;
        r->id = id;
        r->count = count;
        r->got = 0U;
        r->last_len = 0U;
        r->started = now;
    }
    memcpy(r->buf + (size_t)idx * L2_PIECE, payload + L2_FRAG_HDR, n);
    r->got |= (uint8_t)(1U << idx);
    if (idx + 1U == count) {
        r->last_len = n;
    }
    if (r->got == (uint8_t)((1U << count) - 1U)) {
        r->active = 0U;
        *frame = r->buf;
        return (long)((size_t)(count - 1U) * L2_PIECE + r->last_len);
    }
    return 0;
}

static void derive_mac(const uint8_t *data, size_t len, uint8_t mac[6])
{
    uint8_t h[6];

    crypto_blake2b(h, sizeof(h), data, len);
    memcpy(mac, h, 6);
    mac[0] = (uint8_t)((mac[0] & 0xFCU) | 0x02U);   /* local, unicast */
}

void l2_gateway_mac(const uint8_t net_id[32], uint8_t mac[6])
{
    uint8_t in[16 + 32];

    memcpy(in, "camex l2 gateway", 16);
    memcpy(in + 16, net_id, 32);
    derive_mac(in, sizeof(in), mac);
}

void l2_client_mac(const uint8_t net_id[32], const char *client_id,
                   uint8_t mac[6])
{
    uint8_t in[15 + 32 + 64];
    size_t n = 0U;
    size_t i;

    memcpy(in, "camex l2 client", 15);
    memcpy(in + 15, net_id, 32);
    for (i = 0U; client_id[i] != '\0' && n < 64U; ++i, ++n) {
        in[15 + 32 + n] = (uint8_t)tolower((unsigned char)client_id[i]);
    }
    derive_mac(in, 15U + 32U + n, mac);
}

void l2_mac_text(const uint8_t mac[6], char *buf, size_t size)
{
    snprintf(buf, size, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
             mac[2], mac[3], mac[4], mac[5]);
}

int l2_mac_parse(const char *text, uint8_t mac[6])
{
    unsigned int v[6];
    char end;

    if (text == NULL ||
        sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x%c", &v[0], &v[1], &v[2], &v[3],
               &v[4], &v[5], &end) != 6) {
        return -1;
    }
    mac[0] = (uint8_t)v[0];
    mac[1] = (uint8_t)v[1];
    mac[2] = (uint8_t)v[2];
    mac[3] = (uint8_t)v[3];
    mac[4] = (uint8_t)v[4];
    mac[5] = (uint8_t)v[5];
    return 0;
}

/*
 * The MAC table: open addressing, L2_MAC_SLOTS entries; an entry not seen
 * for L2_MAC_AGE seconds is free. A full probe sequence takes the oldest.
 */
#define L2_MAC_SLOTS 4096U
#define L2_MAC_PROBES 16U
#define L2_MAC_AGE 300

typedef struct {
    uint8_t mac[6];
    uint16_t net1;
    uint32_t slot1;          /* 0 = free */
    time_t seen;
} l2_mac_entry_t;

static l2_mac_entry_t mac_table[L2_MAC_SLOTS];

static uint32_t mac_hash(uint16_t net1, const uint8_t mac[6])
{
    uint32_t h = 2166136261U ^ net1;
    size_t i;

    for (i = 0U; i < 6U; ++i) {
        h = (h ^ mac[i]) * 16777619U;
    }
    return h;
}

void l2_mac_learn(uint16_t net1, const uint8_t mac[6], uint32_t slot1,
                  time_t now)
{
    uint32_t h = mac_hash(net1, mac);
    l2_mac_entry_t *victim = NULL;
    uint32_t i;

    if ((mac[0] & 1U) != 0U) {
        return;   /* never a multicast or broadcast source */
    }
    for (i = 0U; i < L2_MAC_PROBES; ++i) {
        l2_mac_entry_t *e = &mac_table[(h + i) % L2_MAC_SLOTS];
        int stale = e->slot1 == 0U ||
                    difftime(now, e->seen) > (double)L2_MAC_AGE;

        if (!stale && e->net1 == net1 && memcmp(e->mac, mac, 6) == 0) {
            e->slot1 = slot1;
            e->seen = now;
            return;
        }
        if (victim == NULL && stale) {
            victim = e;
        }
    }
    if (victim == NULL) {
        for (i = 0U; i < L2_MAC_PROBES; ++i) {
            l2_mac_entry_t *e = &mac_table[(h + i) % L2_MAC_SLOTS];

            if (victim == NULL || e->seen < victim->seen) {
                victim = e;
            }
        }
    }
    memcpy(victim->mac, mac, 6);
    victim->net1 = net1;
    victim->slot1 = slot1;
    victim->seen = now;
}

uint32_t l2_mac_lookup(uint16_t net1, const uint8_t mac[6], time_t now)
{
    uint32_t h = mac_hash(net1, mac);
    uint32_t i;

    for (i = 0U; i < L2_MAC_PROBES; ++i) {
        const l2_mac_entry_t *e = &mac_table[(h + i) % L2_MAC_SLOTS];

        if (e->slot1 != 0U && e->net1 == net1 &&
            memcmp(e->mac, mac, 6) == 0 &&
            difftime(now, e->seen) <= (double)L2_MAC_AGE) {
            return e->slot1;
        }
    }
    return 0U;
}

void l2_mac_forget_slot(uint32_t slot1)
{
    size_t i;

    for (i = 0U; i < L2_MAC_SLOTS; ++i) {
        if (mac_table[i].slot1 == slot1) {
            mac_table[i].slot1 = 0U;
        }
    }
}

void l2_mac_clear(void)
{
    memset(mac_table, 0, sizeof(mac_table));
}

size_t l2_arp_reply(const uint8_t *frame, size_t len, uint32_t ip_be,
                    const uint8_t mac[6], uint8_t *out, size_t out_size)
{
    const uint8_t *arp = frame + L2_ETH_HDR;
    uint8_t *o;

    /* Ethernet II, ARP, Ethernet/IPv4, request for ip_be */
    if (len < L2_ETH_HDR + 28U || out_size < L2_ETH_HDR + 28U ||
        frame[12] != 0x08U || frame[13] != 0x06U ||
        arp[0] != 0x00U || arp[1] != 0x01U || arp[2] != 0x08U ||
        arp[3] != 0x00U || arp[4] != 6U || arp[5] != 4U ||
        arp[6] != 0x00U || arp[7] != 0x01U ||
        memcmp(arp + 24, &ip_be, 4) != 0) {
        return 0U;
    }
    o = out;
    memcpy(o, frame + 6, 6);            /* to the asker */
    memcpy(o + 6, mac, 6);
    o[12] = 0x08U;
    o[13] = 0x06U;
    o += L2_ETH_HDR;
    o[0] = 0x00U; o[1] = 0x01U;         /* Ethernet */
    o[2] = 0x08U; o[3] = 0x00U;         /* IPv4 */
    o[4] = 6U;    o[5] = 4U;
    o[6] = 0x00U; o[7] = 0x02U;         /* reply */
    memcpy(o + 8, mac, 6);              /* sender: the gateway */
    memcpy(o + 14, &ip_be, 4);
    memcpy(o + 18, arp + 8, 6);         /* target: the asker */
    memcpy(o + 24, arp + 14, 4);
    return L2_ETH_HDR + 28U;
}
