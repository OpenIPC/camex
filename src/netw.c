/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * netw.c — named networks: IDs, pools and the addresses of their clients
 *
 */

#include "netw.h"
#include "monocypher.h"
#include "log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <arpa/inet.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

camex_network_t *netw_table = NULL;
static camex_network_t *netw_next = NULL;   /* netw_sync()'s new table */

int netw_alloc(void)
{
    if (netw_table != NULL) {
        memset(netw_table, 0, CAMEX_NET_MAX * sizeof(*netw_table));
        return 0;
    }
    netw_table = calloc(CAMEX_NET_MAX, sizeof(*netw_table));
    netw_next = calloc(CAMEX_NET_MAX, sizeof(*netw_next));
    if (netw_table == NULL || netw_next == NULL) {
        free(netw_table);
        free(netw_next);
        netw_table = NULL;
        netw_next = NULL;
        return -1;
    }
    return 0;
}

#define NETW_ID_CONTEXT "camex network v1"
#define NETW_HS_CONTEXT "camex network handshake v1"
#define NETW_ADDR_CONTEXT "camex network address v1"
#define NETW_POOL_CONTEXT "camex network pool v1"

void netw_id_from_name(const char *name, uint8_t id[CAMEX_NET_ID_LEN])
{
    crypto_blake2b_ctx ctx;

    crypto_blake2b_init(&ctx, CAMEX_NET_ID_LEN);
    crypto_blake2b_update(&ctx, (const uint8_t *)NETW_ID_CONTEXT,
                          sizeof(NETW_ID_CONTEXT) - 1U);
    crypto_blake2b_update(&ctx, (const uint8_t *)name, strlen(name));
    crypto_blake2b_final(&ctx, id);
}

int netw_name_valid(const char *name)
{
    size_t len;
    size_t i;

    if (name == NULL) {
        return 0;
    }
    len = strlen(name);
    if (len == 0U || len > CAMEX_NET_NAME_MAX) {
        return 0;
    }
    for (i = 0U; i < len; ++i) {
        unsigned char c = (unsigned char)name[i];

        if (c < 0x20U || c == 0x7FU) {
            return 0;
        }
    }
    return 1;
}

void netw_handshake_key(uint8_t key[32])
{
    crypto_blake2b(key, 32U, (const uint8_t *)NETW_HS_CONTEXT,
                   sizeof(NETW_HS_CONTEXT) - 1U);
}

void netw_default_label(const uint8_t id[CAMEX_NET_ID_LEN], char *out,
                        size_t size)
{
    snprintf(out, size, "net-%02x%02x%02x%02x", id[0], id[1], id[2], id[3]);
}

void netw_label_from_name(const char *name, char *out, size_t size)
{
    size_t n;

    if (size == 0U) {
        return;
    }
    n = strlen(name);
    if (n >= size) {
        n = size - 1U;
        /* Back to the start of a character */
        while (n > 0U && ((unsigned char)name[n] & 0xC0U) == 0x80U) {
            --n;
        }
    }
    memcpy(out, name, n);
    out[n] = '\0';
}

void netw_name_label(int n, const uint8_t id[CAMEX_NET_ID_LEN],
                     const char *name)
{
    uint8_t check[CAMEX_NET_ID_LEN];

    if (netw_table == NULL || n < 0 || n >= (int)CAMEX_NET_MAX ||
        name == NULL || name[0] == '\0' || netw_table[n].listed) {
        return;
    }
    netw_id_from_name(name, check);
    if (memcmp(check, id, sizeof(check)) == 0) {
        netw_label_from_name(name, netw_table[n].label,
                             sizeof(netw_table[n].label));
    }
    crypto_wipe(check, sizeof(check));
}

int netw_find(const uint8_t id[CAMEX_NET_ID_LEN])
{
    size_t i;

    if (netw_table == NULL) {
        return -1;
    }
    for (i = 0U; i < CAMEX_NET_MAX; ++i) {
        if (netw_table[i].active &&
            memcmp(netw_table[i].id, id, CAMEX_NET_ID_LEN) == 0) {
            return (int)i;
        }
    }
    return -1;
}

const char *netw_label(int n)
{
    if (n < 0 || n >= (int)CAMEX_NET_MAX) {
        return "";
    }
    return netw_table[n].label;
}

static uint32_t prefix_mask(uint8_t prefix)
{
    return prefix == 0U ? 0U : 0xFFFFFFFFU << (32U - prefix);
}

/* Do a/ap and b/bp share an address? */
static int ranges_overlap(uint32_t a, uint8_t ap, uint32_t b, uint8_t bp)
{
    uint32_t m = prefix_mask(ap < bp ? ap : bp);

    return (a & m) == (b & m);
}

static uint32_t pool_size_of(uint8_t prefix)
{
    return prefix == 0U ? 0U : (uint32_t)1U << (32U - prefix);
}

uint32_t netw_gateway(int n)
{
    return netw_table[n].pool_net + 1U;
}

uint32_t netw_first(int n)
{
    return netw_table[n].pool_net + 2U;
}

uint32_t netw_last(int n)
{
    return netw_table[n].pool_net + pool_size_of(netw_table[n].pool_prefix) -
           2U;
}

int netw_has(int n, uint32_t ip_be)
{
    uint32_t host = ntohl(ip_be);

    if (n < 0 || n >= (int)CAMEX_NET_MAX || !netw_table[n].active) {
        return 0;
    }
    return ip_be != 0U && host >= netw_first(n) && host <= netw_last(n);
}

int netw_of_ip(uint32_t ip_be)
{
    uint32_t host = ntohl(ip_be);
    size_t i;

    if (netw_table == NULL) {
        return -1;
    }
    for (i = 0U; i < CAMEX_NET_MAX; ++i) {
        const camex_network_t *t = &netw_table[i];

        if (t->active &&
            (host & prefix_mask(t->pool_prefix)) == t->pool_net) {
            return (int)i;
        }
    }
    return -1;
}

/* The hash's position of client_id among the hosts of network n */
static uint32_t netw_base(int n, const char *client_id, uint32_t hosts)
{
    crypto_blake2b_ctx ctx;
    uint8_t h[8];
    char lower[64];
    size_t i;
    uint64_t v = 0U;

    /* IDs compare without case everywhere: hash them the same way */
    for (i = 0U; client_id[i] != '\0' && i < sizeof(lower) - 1U; ++i) {
        lower[i] = (char)tolower((unsigned char)client_id[i]);
    }
    lower[i] = '\0';
    crypto_blake2b_init(&ctx, sizeof(h));
    crypto_blake2b_update(&ctx, (const uint8_t *)NETW_ADDR_CONTEXT,
                          sizeof(NETW_ADDR_CONTEXT) - 1U);
    crypto_blake2b_update(&ctx, netw_table[n].id, CAMEX_NET_ID_LEN);
    crypto_blake2b_update(&ctx, (const uint8_t *)lower, strlen(lower));
    crypto_blake2b_final(&ctx, h);
    for (i = 0U; i < sizeof(h); ++i) {
        v = (v << 8) | h[i];
    }
    return (uint32_t)(v % hosts);
}

/* 0, +1, -1, +2, -2, ... */
static int64_t probe_offset(unsigned int k)
{
    int64_t step = (int64_t)((k + 1U) / 2U);

    return (k % 2U) == 1U ? step : -step;
}

uint32_t netw_candidate(int n, const char *client_id, unsigned int k)
{
    uint32_t first;
    uint32_t hosts;
    int64_t pos;

    if (n < 0 || n >= (int)CAMEX_NET_MAX || !netw_table[n].active ||
        client_id == NULL || client_id[0] == '\0') {
        return 0U;
    }
    first = netw_first(n);
    hosts = netw_last(n) - first + 1U;
    if (hosts == 0U || k >= hosts) {
        return 0U;
    }
    pos = ((int64_t)netw_base(n, client_id, hosts) + probe_offset(k)) %
          (int64_t)hosts;
    if (pos < 0) {
        pos += hosts;
    }
    return htonl(first + (uint32_t)pos);
}

int netw_candidate_rank(int n, const char *client_id, uint32_t ip_be)
{
    unsigned int k;

    for (k = 0U; k < CAMEX_NET_PROBES; ++k) {
        uint32_t c = netw_candidate(n, client_id, k);

        if (c == 0U) {
            break;
        }
        if (c == ip_be) {
            return (int)k;
        }
    }
    return -1;
}

void netw_pool_text(int n, char *buf, size_t size)
{
    struct in_addr a;
    char ip[16];

    a.s_addr = htonl(netw_table[n].pool_net);
    inet_ntop(AF_INET, &a, ip, sizeof(ip));
    snprintf(buf, size, "%s/%u", ip, (unsigned)netw_table[n].pool_prefix);
}

uint32_t netw_pool_index(const uint8_t id[CAMEX_NET_ID_LEN], uint32_t pools)
{
    uint8_t h[8];
    uint64_t v = 0U;
    size_t i;
    crypto_blake2b_ctx ctx;

    crypto_blake2b_init(&ctx, sizeof(h));
    crypto_blake2b_update(&ctx, (const uint8_t *)NETW_POOL_CONTEXT,
                          sizeof(NETW_POOL_CONTEXT) - 1U);
    crypto_blake2b_update(&ctx, id, CAMEX_NET_ID_LEN);
    crypto_blake2b_final(&ctx, h);
    for (i = 0U; i < sizeof(h); ++i) {
        v = (v << 8) | h[i];
    }
    return pools == 0U ? 0U : (uint32_t)(v % pools);
}

/* Settings of the last netw_sync(), for netw_create() */
static uint32_t sync_supernet = CAMEX_NET_SUPERNET_DEFAULT_NET;
static uint8_t sync_super_prefix = CAMEX_NET_SUPERNET_DEFAULT_PREFIX;
static uint8_t sync_pool_prefix = CAMEX_NET_POOL_PREFIX_DEFAULT;
static uint32_t sync_reserved[8];
static size_t sync_reserved_count = 0U;

/* Is net/prefix free in table t (slot `self` aside) and of the reserved
 * ranges? */
static int pool_free(const camex_network_t *t, size_t self, uint32_t net,
                     uint8_t prefix, const uint32_t *reserved,
                     size_t reserved_count)
{
    size_t i;

    for (i = 0U; i < reserved_count; ++i) {
        if (ranges_overlap(net, prefix, reserved[2U * i],
                           (uint8_t)reserved[2U * i + 1U])) {
            return 0;
        }
    }
    for (i = 0U; i < CAMEX_NET_MAX; ++i) {
        if (i == self || !t[i].active || t[i].pool_prefix == 0U) {
            continue;
        }
        if (ranges_overlap(net, prefix, t[i].pool_net, t[i].pool_prefix)) {
            return 0;
        }
    }
    return 1;
}

/* A derived pool for slot s of t: its old one while still free, else the
 * ID's choice among the supernet's pools, or the next free one. -1 if
 * none is free. */
static int derive_pool(camex_network_t *t, size_t s, uint32_t supernet,
                       uint8_t super_prefix, uint8_t pool_prefix,
                       const uint32_t *reserved, size_t reserved_count)
{
    uint32_t pools = (uint32_t)1U << (pool_prefix - super_prefix);
    uint32_t start = netw_pool_index(t[s].id, pools);
    uint32_t i;
    uint32_t old_net = t[s].pool_net;
    uint8_t old_prefix = t[s].pool_prefix;

    t[s].pool_prefix = 0U;   /* not in the way of its own search */
    if (old_prefix == pool_prefix &&
        (old_net & prefix_mask(super_prefix)) == supernet &&
        pool_free(t, s, old_net, pool_prefix, reserved, reserved_count)) {
        t[s].pool_net = old_net;
        t[s].pool_prefix = pool_prefix;
        return 0;
    }
    for (i = 0U; i < pools; ++i) {
        uint32_t idx = (start + i) % pools;
        uint32_t net = supernet + (idx << (32U - pool_prefix));

        if (pool_free(t, s, net, pool_prefix, reserved, reserved_count)) {
            t[s].pool_net = net;
            t[s].pool_prefix = pool_prefix;
            return 0;
        }
    }
    return -1;
}

int netw_sync(const camex_net_conf_t *conf, size_t count, int allow_new,
              uint32_t supernet, uint8_t super_prefix, uint8_t pool_prefix,
              const uint32_t *reserved, size_t reserved_count)
{
    camex_network_t *next;
    int slot_of[CAMEX_NET_MAX];
    size_t i;
    size_t j;

    if (count > CAMEX_NET_MAX || pool_prefix <= super_prefix ||
        pool_prefix > 30U || reserved_count > 4U) {
        log_message(LOG_ERR, "Invalid network settings");
        return -1;
    }
    if (netw_table == NULL && netw_alloc() != 0) {
        log_message(LOG_ERR, "No memory for the networks");
        return -1;
    }
    next = netw_next;
    memcpy(next, netw_table, CAMEX_NET_MAX * sizeof(*next));
    /* Every slot starts unused; listed networks take theirs back */
    for (i = 0U; i < CAMEX_NET_MAX; ++i) {
        if (next[i].active && !next[i].listed && allow_new) {
            continue;   /* a network a client created stays */
        }
        next[i].active = 0U;
    }
    for (j = 0U; j < count; ++j) {
        int s = -1;
        uint8_t alias_up;
        uint32_t alias_ip;

        /* Its slot: one this sync gave it already (listed twice), a kept
         * network with its ID, its old slot, else a free one (never used
         * before first, so a slot keeps its network as long as it can) */
        for (i = 0U; i < CAMEX_NET_MAX && s < 0; ++i) {
            if (next[i].active &&
                memcmp(next[i].id, conf[j].id, CAMEX_NET_ID_LEN) == 0) {
                s = (int)i;
            }
        }
        if (s >= 0 && next[s].listed) {
            log_message(LOG_ERR, "Network %s is listed twice",
                        next[s].label);
            return -1;
        }
        for (i = 0U; i < CAMEX_NET_MAX && s < 0; ++i) {
            if (!next[i].active && netw_table[i].created != 0 &&
                memcmp(netw_table[i].id, conf[j].id, CAMEX_NET_ID_LEN) == 0) {
                s = (int)i;
            }
        }
        for (i = 0U; i < CAMEX_NET_MAX && s < 0; ++i) {
            if (!next[i].active && next[i].created == 0 &&
                !next[i].alias_up) {
                s = (int)i;
            }
        }
        for (i = 0U; i < CAMEX_NET_MAX && s < 0; ++i) {
            if (!next[i].active) {
                s = (int)i;
            }
        }
        if (s < 0) {
            log_message(LOG_ERR, "More than %u networks",
                        (unsigned)CAMEX_NET_MAX);
            return -1;
        }
        if (memcmp(next[s].id, conf[j].id, CAMEX_NET_ID_LEN) != 0) {
            /* The slot changes hands: the server still has to take the
             * old gateway address off the tunnel */
            alias_up = next[s].alias_up;
            alias_ip = next[s].alias_ip;
            memset(&next[s], 0, sizeof(next[s]));
            next[s].alias_up = alias_up;
            next[s].alias_ip = alias_ip;
            memcpy(next[s].id, conf[j].id, CAMEX_NET_ID_LEN);
        }
        next[s].active = 1U;
        next[s].listed = 1U;
        next[s].isolation = conf[j].isolation;
        next[s].tap = conf[j].tap;
        next[s].noaddr = conf[j].noaddr;
        next[s].mesh = conf[j].mesh;
        if (conf[j].noaddr && !conf[j].tap) {
            log_message(LOG_ERR, "addresses = none is for a TAP network "
                        "(mode = tap)");
            return -1;
        }
        if (conf[j].label[0] != '\0') {
            snprintf(next[s].label, sizeof(next[s].label), "%s",
                     conf[j].label);
        } else {
            netw_default_label(conf[j].id, next[s].label,
                               sizeof(next[s].label));
        }
        if (next[s].created == 0) {
            next[s].created = time(NULL);
        }
        next[s].pool_set = conf[j].pool_set;
        if (conf[j].pool_set) {
            next[s].pool_net = conf[j].pool_net;
            next[s].pool_prefix = conf[j].pool_prefix;
        }
        slot_of[j] = s;
    }
    /* Explicit pools first: they must not overlap anything */
    for (j = 0U; j < count; ++j) {
        size_t s = (size_t)slot_of[j];
        uint8_t p = next[s].pool_prefix;
        uint32_t net = next[s].pool_net;

        if (!conf[j].pool_set) {
            continue;
        }
        next[s].pool_prefix = 0U;
        if (!pool_free(next, s, net, p, reserved, reserved_count)) {
            char text[24];
            struct in_addr a;

            a.s_addr = htonl(net);
            inet_ntop(AF_INET, &a, text, sizeof(text));
            log_message(LOG_ERR, "pool= %s/%u of network %s overlaps another "
                        "network, the auto pool or the server's tunnel "
                        "network", text, (unsigned)p, next[s].label);
            return -1;
        }
        next[s].pool_prefix = p;
    }
    /* Derived pools: listed networks in file order, then the created ones
     * by slot; each keeps its old pool while it is still free */
    for (i = 0U; i < CAMEX_NET_MAX; ++i) {
        if (next[i].active && !next[i].pool_set) {
            next[i].pool_prefix = next[i].pool_prefix == pool_prefix &&
                                  (next[i].pool_net &
                                   prefix_mask(super_prefix)) == supernet
                                      ? next[i].pool_prefix : 0U;
        }
    }
    for (j = 0U; j < count + CAMEX_NET_MAX; ++j) {
        size_t s;

        if (j < count) {
            s = (size_t)slot_of[j];
            if (conf[j].pool_set) {
                continue;
            }
        } else {
            s = j - count;
            if (!next[s].active || next[s].listed) {
                continue;
            }
        }
        if (derive_pool(next, s, supernet, super_prefix, pool_prefix,
                        reserved, reserved_count) != 0) {
            log_message(LOG_ERR, "No free /%u in the network supernet for "
                        "network %s", (unsigned)pool_prefix, next[s].label);
            return -1;
        }
    }
    memcpy(netw_table, next, CAMEX_NET_MAX * sizeof(*netw_table));
    sync_supernet = supernet;
    sync_super_prefix = super_prefix;
    sync_pool_prefix = pool_prefix;
    sync_reserved_count = reserved_count;
    memcpy(sync_reserved, reserved, reserved_count * 2U * sizeof(uint32_t));
    return 0;
}

int netw_create(const uint8_t id[CAMEX_NET_ID_LEN])
{
    size_t i;
    int s = -1;

    if (netw_table == NULL) {
        return -1;
    }
    for (i = 0U; i < CAMEX_NET_MAX && s < 0; ++i) {
        if (!netw_table[i].active && !netw_table[i].alias_up) {
            s = (int)i;
        }
    }
    if (s < 0) {
        return -1;
    }
    memset(&netw_table[s], 0, sizeof(netw_table[s]));
    memcpy(netw_table[s].id, id, CAMEX_NET_ID_LEN);
    netw_default_label(id, netw_table[s].label, sizeof(netw_table[s].label));
    netw_table[s].isolation = -1;
    netw_table[s].created = time(NULL);
    netw_table[s].active = 1U;
    if (derive_pool(netw_table, (size_t)s, sync_supernet, sync_super_prefix,
                    sync_pool_prefix, sync_reserved,
                    sync_reserved_count) != 0) {
        memset(&netw_table[s], 0, sizeof(netw_table[s]));
        return -1;
    }
    return s;
}
