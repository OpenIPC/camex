/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * server.c — server side of protocol v2: client table, handshakes,
 *            sessions, packet forwarding
 *
 */

#include "server.h"
#include "client.h"
#include "crypto.h"
#include "keys.h"
#include "monocypher.h"
#include "noise.h"
#include "session.h"
#include "log.h"
#include "util.h"
#include "proto.h"
#include "net.h"
#include "packet.h"
#include "tun.h"
#include "config.h"
#include "camex.h"
#include "slotidx.h"
#include "version.h"

#ifndef _WIN32
#include <arpa/inet.h>
#endif
#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifndef _WIN32
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#define close(fd) closesocket(fd)
#endif

server_client_t *server_clients = NULL;
size_t server_max_clients = 0U;

/* Where the search for a free slot starts: after the last slot handed out,
 * so a table with live clients in its low slots is not walked from 0 on
 * every new client (#75) */
static size_t alloc_next = 0U;

/* Indexes of the live table (#75): tunnel IP, client ID, peer address.
 * Entries are added when a key is set (server_set_ip/_id/_addr); stale
 * ones (a freed or reused slot) are skipped by the match functions and
 * dropped when the index is rebuilt. Without memory for an index the
 * lookups fall back to a scan (idx_ok = 0). */
static slotidx_t idx_ip;
static slotidx_t idx_id;
static slotidx_t idx_addr;
static int idx_ok = 0;

/* Auto pool (#15, #75): server_db.globals.pool_net/pool_prefix; the first
 * address is the gateway, clients get the second to the last but one. */
static uint32_t auto_pool_next = 0U;   /* host order, 0 = pool start */
/* When the pool was last found full (and which pool): until the next
 * second further handshakes do not walk it again */
static time_t pool_full_at = 0;
static uint32_t pool_full_net = 0U;
static uint8_t pool_full_prefix = 0U;

static uint32_t pool_size(void)
{
    return 1U << (32U - server_db.globals.pool_prefix);
}

static uint32_t pool_gateway(void)
{
    return server_db.globals.pool_net + 1U;
}

static uint32_t pool_first(void)
{
    return server_db.globals.pool_net + 2U;
}

static uint32_t pool_last(void)
{
    return server_db.globals.pool_net + pool_size() - 2U;
}

/* host-order address -> dotted text */
static void ip_text(uint32_t host, char *buf, size_t size)
{
    struct in_addr a;

    a.s_addr = htonl(host);
    inet_ntop(AF_INET, &a, buf, (socklen_t)size);
}

/* Is ip_be (network order) a client address of the pool? */
static int pool_has(uint32_t ip_be)
{
    uint32_t host = ntohl(ip_be);

    return ip_be != 0U && host >= pool_first() && host <= pool_last();
}

size_t server_default_max_clients(void)
{
    size_t hosts = (size_t)(pool_last() - pool_first() + 1U);

    if (hosts > CAMEX_MAX_CLIENTS_LIMIT) {
        hosts = CAMEX_MAX_CLIENTS_LIMIT;
    }
    return hosts > CAMEX_MAX_CLIENTS_DEFAULT ? hosts
                                             : CAMEX_MAX_CLIENTS_DEFAULT;
}

/* The server's own tunnel address (network order), never given to a
 * client (#75); set by server_check_pool() at startup and reload */
static uint32_t server_tunnel_ip = 0U;

/*
 * Leases (#81): an auto client whose entry ends (it expired, was evicted
 * for a slot, its TCP connection closed) keeps its address under its ID.
 * The pool gives that address to another client only when it has no
 * other free one and the lease is older than SERVER_LEASE_MIN (a client
 * that is still alive without the server knowing comes back within a
 * rekey interval); the client gets it back when it returns. One lease per
 * slot; when they are all taken the oldest one goes.
 */
#define SERVER_LEASE_MIN (V2_REKEY_AFTER_TIME + CAMEX_CLIENT_TIMEOUT)
#define SERVER_LEASE_MAX (1U << 20)   /* the table grows up to this */

typedef struct {
    char id[CAMEX_CLIENT_ID_LEN];
    uint32_t ip_be;        /* 0 = unused */
    time_t since;
    uint8_t prev;          /* the address of its previous session: kept
                            * for it, never handed back as its address */
} server_lease_t;

static server_lease_t *server_leases = NULL;
static size_t server_lease_cap = 0U;
static slotidx_t idx_lease_ip;
static slotidx_t idx_lease_id;
static int lease_idx_ok = 0;
static size_t server_lease_used = 0U;   /* leases in the table */
static size_t server_lease_next = 0U;   /* where a free one is looked for */
static time_t server_started = 0;   /* for the pool's start hold (#81) */

void server_check_pool(const char *server_cidr)
{
    char ip[16];
    char mask[16];
    char gw[16];
    struct in_addr a;
    struct in_addr m;
    uint32_t sip;
    uint32_t smask;
    uint32_t pmask = 0xFFFFFFFFU << (32U - server_db.globals.pool_prefix);

    server_tunnel_ip = 0U;
    if (server_cidr == NULL || server_cidr[0] == '\0' ||
        parse_local_cidr(server_cidr, ip, sizeof(ip), mask,
                         sizeof(mask)) != 0 ||
        inet_pton(AF_INET, ip, &a) != 1 || inet_pton(AF_INET, mask, &m) != 1) {
        return;
    }
    server_tunnel_ip = a.s_addr;
    sip = ntohl(a.s_addr);
    smask = ntohl(m.s_addr);
    ip_text(pool_gateway(), gw, sizeof(gw));
    if ((sip & smask) != (server_db.globals.pool_net & smask) ||
        smask > pmask) {
        char net[16];

        ip_text(server_db.globals.pool_net, net, sizeof(net));
        log_message(LOG_WARNING, "Auto pool %s/%u is not inside the server's "
                    "tunnel network %s: the server has no route to the "
                    "clients it gives these addresses", net,
                    (unsigned)server_db.globals.pool_prefix, server_cidr);
    }
    if (sip != pool_gateway()) {
        log_message(LOG_WARNING, "Auto pool clients get the gateway %s, but "
                    "the server's tunnel address is %s", gw, ip);
    }
}

/* A pending address (#77) is the client's too: forwarding and the pool
 * see it as taken */
static int match_ip(uint32_t slot, const void *key)
{
    const uint32_t ip = *(const uint32_t *)key;

    const server_client_t *c;

    if (ip == 0U || slot >= server_max_clients ||
        !server_clients[slot].active) {
        return 0;
    }
    c = &server_clients[slot];
    /* The previous session's address stays taken while that session still
     * receives (#77): its packets come from that address */
    return c->ip_be == ip || c->pending_ip_be == ip ||
           (c->prev.valid && c->prev_ip_be == ip);
}

/* Only the current address: its owner wins over a client that has it as
 * a pending or previous address (#77) */
static int match_cur_ip(uint32_t slot, const void *key)
{
    const uint32_t ip = *(const uint32_t *)key;

    return ip != 0U && slot < server_max_clients &&
           server_clients[slot].active && server_clients[slot].ip_be == ip;
}

static int match_id(uint32_t slot, const void *key)
{
    return slot < server_max_clients && server_clients[slot].active &&
           strcasecmp(server_clients[slot].client_id,
                      (const char *)key) == 0;
}

static int match_addr(uint32_t slot, const void *key)
{
    return slot < server_max_clients && server_clients[slot].active &&
           net_sockaddr_equal(&server_clients[slot].addr,
                              (const struct sockaddr_in *)key);
}

static uint32_t hash_addr(const struct sockaddr_in *addr)
{
    uint8_t k[6];

    memcpy(k, &addr->sin_addr.s_addr, 4);
    memcpy(k + 4, &addr->sin_port, 2);
    return slotidx_hash_bytes(k, sizeof(k));
}

static int addr_is_set(const struct sockaddr_in *addr)
{
    return addr->sin_addr.s_addr != 0U || addr->sin_port != 0U;
}

static int match_lease_ip(uint32_t slot, const void *key)
{
    const uint32_t ip = *(const uint32_t *)key;

    return ip != 0U && slot < server_lease_cap &&
           server_leases[slot].ip_be == ip;
}

static int match_lease_id(uint32_t slot, const void *key)
{
    return slot < server_lease_cap && server_leases[slot].ip_be != 0U &&
           !server_leases[slot].prev &&
           strcasecmp(server_leases[slot].id, (const char *)key) == 0;
}

static void server_lease_index_rebuild(void)
{
    size_t i;

    lease_idx_ok = slotidx_reset(&idx_lease_ip, server_lease_cap) == 0 &&
                   slotidx_reset(&idx_lease_id, server_lease_cap) == 0;
    for (i = 0U; lease_idx_ok && i < server_lease_cap; ++i) {
        const server_lease_t *l = &server_leases[i];

        if (l->ip_be != 0U &&
            (slotidx_insert(&idx_lease_ip, slotidx_hash_u32(l->ip_be),
                            (uint32_t)i) != 0 ||
             slotidx_insert(&idx_lease_id, slotidx_hash_nocase(l->id),
                            (uint32_t)i) != 0)) {
            lease_idx_ok = 0;
        }
    }
}

static server_lease_t *server_lease_by_ip(uint32_t ip_be)
{
    size_t i;

    if (ip_be == 0U) {
        return NULL;
    }
    if (lease_idx_ok) {
        long k = slotidx_find(&idx_lease_ip, slotidx_hash_u32(ip_be),
                              match_lease_ip, &ip_be);

        return k >= 0 ? &server_leases[k] : NULL;
    }
    for (i = 0U; i < server_lease_cap; ++i) {
        if (match_lease_ip((uint32_t)i, &ip_be)) {
            return &server_leases[i];
        }
    }
    return NULL;
}

static server_lease_t *server_lease_by_id(const char *client_id)
{
    size_t i;

    if (client_id == NULL || client_id[0] == '\0') {
        return NULL;
    }
    if (lease_idx_ok) {
        long k = slotidx_find(&idx_lease_id, slotidx_hash_nocase(client_id),
                              match_lease_id, client_id);

        return k >= 0 ? &server_leases[k] : NULL;
    }
    for (i = 0U; i < server_lease_cap; ++i) {
        if (match_lease_id((uint32_t)i, client_id)) {
            return &server_leases[i];
        }
    }
    return NULL;
}

static void server_lease_drop(server_lease_t *l)
{
    if (l != NULL && l->ip_be != 0U) {
        memset(l, 0, sizeof(*l));   /* its index entries go stale */
        server_lease_used--;
    }
}

/* The lease of ip_be leaves room for client_id: none, or its own */
static int server_lease_allows(uint32_t ip_be, const char *client_id)
{
    const server_lease_t *l = server_lease_by_ip(ip_be);

    return l == NULL || (client_id != NULL &&
                         strcasecmp(l->id, client_id) == 0);
}

/* prev: the address of the client's previous session, kept as well */
static void server_lease_add(const char *client_id, uint32_t ip_be,
                             int prev)
{
    server_lease_t *l = NULL;
    size_t i;

    if (server_lease_cap == 0U || ip_be == 0U || client_id == NULL ||
        client_id[0] == '\0') {
        return;
    }
    if (!prev) {
        server_lease_drop(server_lease_by_id(client_id));
    }
    server_lease_drop(server_lease_by_ip(ip_be));
    if (server_lease_used < server_lease_cap) {
        /* A free one, from a cursor: many clients ending at once (an
         * outage) cost one step each, not a scan each */
        for (i = 0U; i < server_lease_cap; ++i) {
            size_t k = (server_lease_next + i) % server_lease_cap;

            if (server_leases[k].ip_be == 0U) {
                l = &server_leases[k];
                server_lease_next = (k + 1U) % server_lease_cap;
                break;
            }
        }
    }
    if (l == NULL) {
        for (i = 0U; i < server_lease_cap; ++i) {
            if (l == NULL || server_leases[i].since < l->since) {
                l = &server_leases[i];   /* the oldest */
            }
        }
    }
    /* All taken and the oldest is still young: grow the table, up to the
     * pool's size, rather than give a young lease up (#81) */
    if (l->ip_be != 0U &&
        difftime(g_now, l->since) < (double)SERVER_LEASE_MIN) {
        size_t limit = pool_size() > SERVER_LEASE_MAX ? SERVER_LEASE_MAX
                                                      : pool_size();
        size_t cap = server_lease_cap * 2U > limit ? limit
                                                   : server_lease_cap * 2U;
        server_lease_t *grown = cap > server_lease_cap
                                    ? realloc(server_leases,
                                              cap * sizeof(*grown))
                                    : NULL;

        if (grown != NULL) {
            memset(grown + server_lease_cap, 0,
                   (cap - server_lease_cap) * sizeof(*grown));
            l = grown + server_lease_cap;
            server_lease_next = server_lease_cap + 1U;
            server_leases = grown;
            server_lease_cap = cap;
            lease_idx_ok = 0;   /* rebuilt below for the new size */
        } else {
            char ip[16];

            ip_text(ntohl(l->ip_be), ip, sizeof(ip));
            log_message(LOG_WARNING, "Lease table full: the address %s of "
                        "client %s (gone for %.0f s) is no longer kept",
                        ip, l->id, difftime(g_now, l->since));
        }
    }
    if (l->ip_be == 0U) {
        server_lease_used++;
    }
    snprintf(l->id, sizeof(l->id), "%s", client_id);
    l->ip_be = ip_be;
    l->since = g_now != 0 ? g_now : time(NULL);
    l->prev = prev ? 1U : 0U;
    if (!lease_idx_ok ||
        slotidx_crowded(&idx_lease_ip, server_lease_cap) ||
        slotidx_crowded(&idx_lease_id, server_lease_cap) ||
        slotidx_insert(&idx_lease_ip, slotidx_hash_u32(ip_be),
                       (uint32_t)(l - server_leases)) != 0 ||
        slotidx_insert(&idx_lease_id, slotidx_hash_nocase(l->id),
                       (uint32_t)(l - server_leases)) != 0) {
        server_lease_index_rebuild();
    }
}

/* Seconds after the start in which the auto pool hands out only the
 * address a client asks for (its previous one): the leases did not
 * survive the restart, and clients come back within a few seconds
 * (#81). SERVER_POOL_HOLD; CAMEX_POOL_HOLD (0..600) changes it, for
 * tests. */
static int server_pool_holding(void)
{
    static int value = -1;

    if (value < 0) {
        const char *env = getenv("CAMEX_POOL_HOLD");
        int v = env != NULL ? atoi(env) : -1;

        value = (env != NULL && v >= 0 && v <= 600) ? v : SERVER_POOL_HOLD;
    }
    return server_started != 0 &&
           difftime(g_now, server_started) < (double)value;
}

/* Rebuild all three indexes from the table */
static void server_index_rebuild(void)
{
    size_t i;

    idx_ok = slotidx_reset(&idx_ip, server_max_clients) == 0 &&
             slotidx_reset(&idx_id, server_max_clients) == 0 &&
             slotidx_reset(&idx_addr, server_max_clients) == 0;
    for (i = 0U; idx_ok && i < server_max_clients; ++i) {
        const server_client_t *c = &server_clients[i];

        if (!c->active) {
            continue;
        }
        if ((c->ip_be != 0U &&
             slotidx_insert(&idx_ip, slotidx_hash_u32(c->ip_be),
                            (uint32_t)i) != 0) ||
            (c->pending_ip_be != 0U &&
             slotidx_insert(&idx_ip, slotidx_hash_u32(c->pending_ip_be),
                            (uint32_t)i) != 0) ||
            (c->prev.valid && c->prev_ip_be != 0U &&
             slotidx_insert(&idx_ip, slotidx_hash_u32(c->prev_ip_be),
                            (uint32_t)i) != 0) ||
            (c->client_id[0] != '\0' &&
             slotidx_insert(&idx_id, slotidx_hash_nocase(c->client_id),
                            (uint32_t)i) != 0) ||
            (addr_is_set(&c->addr) &&
             slotidx_insert(&idx_addr, hash_addr(&c->addr),
                            (uint32_t)i) != 0)) {
            idx_ok = 0;
        }
    }
    if (!idx_ok) {
        log_message(LOG_WARNING, "Out of memory for the client indexes: "
                    "lookups scan the table");
    }
}

/* Add slot under h to x; rebuilds when stale entries crowd it */
static void server_index_add(slotidx_t *x, uint32_t h, const server_client_t *c)
{
    if (!idx_ok) {
        return;
    }
    /* The address index holds up to three keys a client: current,
     * pending and previous (#77) */
    if (slotidx_crowded(x, x == &idx_ip ? server_max_clients * 3U
                                         : server_max_clients)) {
        server_index_rebuild();   /* takes the new key from the table */
        return;
    }
    if (slotidx_insert(x, h, (uint32_t)(c - server_clients)) != 0) {
        server_index_rebuild();
    }
}

void server_set_ip(server_client_t *entry, uint32_t ip_be)
{
    if (entry->ip_be == ip_be) {
        return;
    }
    entry->ip_be = ip_be;
    if (ip_be != 0U) {
        server_index_add(&idx_ip, slotidx_hash_u32(ip_be), entry);
    }
}

/* The address an auto client gets at a handshake (#77). A client with a
 * working session keeps its address until it confirms the new session
 * (its first packet on it, server_handle_data()): until then the new one
 * is pending, reserved for it. Otherwise it is taken at once. */
static void server_assign_ip(server_client_t *entry, uint32_t ip_be)
{
    if (entry->cur.valid && entry->ip_be != 0U && ip_be != entry->ip_be) {
        if (entry->pending_ip_be != ip_be) {
            entry->pending_ip_be = ip_be;
            server_index_add(&idx_ip, slotidx_hash_u32(ip_be), entry);
        }
        return;
    }
    entry->pending_ip_be = 0U;
    server_set_ip(entry, ip_be);
}

/* Put an entry's addresses back after a handshake that failed (#77): the
 * current one and the pending one, both indexed again (an index rebuild
 * in between may have dropped them) */
static void server_restore_ip(server_client_t *entry, uint32_t ip_be,
                              uint32_t pending_ip_be)
{
    server_set_ip(entry, ip_be);
    entry->pending_ip_be = pending_ip_be;
    if (ip_be != 0U) {
        server_index_add(&idx_ip, slotidx_hash_u32(ip_be), entry);
    }
    if (pending_ip_be != 0U) {
        server_index_add(&idx_ip, slotidx_hash_u32(pending_ip_be), entry);
    }
}

void server_set_id(server_client_t *entry, const char *client_id)
{
    if (strcmp(entry->client_id, client_id) == 0) {
        return;
    }
    snprintf(entry->client_id, sizeof(entry->client_id), "%s", client_id);
    if (entry->client_id[0] != '\0') {
        server_index_add(&idx_id, slotidx_hash_nocase(entry->client_id),
                         entry);
    }
}

void server_set_addr(server_client_t *entry, const struct sockaddr_in *addr)
{
    if (net_sockaddr_equal(&entry->addr, addr) && addr_is_set(addr)) {
        return;
    }
    entry->addr = *addr;
    if (addr_is_set(addr)) {
        server_index_add(&idx_addr, hash_addr(addr), entry);
    }
}

int server_clients_init(size_t count)
{
    size_t i;

    server_clients_free();
    {
        char what[64];

        snprintf(what, sizeof(what), "%u client slots", (unsigned)count);
        server_clients = key_locked_calloc(count, sizeof(*server_clients),
                                           what);
    }
    if (server_clients == NULL) {
        log_message(LOG_ERR, "Out of memory for %u client slots",
                    (unsigned)count);
        return -1;
    }
    server_max_clients = count;
    alloc_next = 0U;
    for (i = 0U; i < count; ++i) {
        server_clients[i].tcp_fd = -1;
    }
    auto_pool_next = 0U;
    server_index_rebuild();
    /* One lease per slot (#81) */
    server_leases = calloc(count, sizeof(*server_leases));
    server_lease_cap = server_leases != NULL ? count : 0U;
    server_lease_used = 0U;
    server_lease_next = 0U;
    server_lease_index_rebuild();
    server_started = time(NULL);
    return 0;
}

void server_clients_free(void)
{
    if (server_clients != NULL) {
        crypto_wipe(server_clients,
                    server_max_clients * sizeof(*server_clients));
        free(server_clients);
    }
    server_clients = NULL;
    server_max_clients = 0U;
    slotidx_free(&idx_ip);
    slotidx_free(&idx_id);
    slotidx_free(&idx_addr);
    idx_ok = 0;
    free(server_leases);
    server_leases = NULL;
    server_lease_cap = 0U;
    slotidx_free(&idx_lease_ip);
    slotidx_free(&idx_lease_id);
    lease_idx_ok = 0;
}

static const char *profile_owner_of_ip(uint32_t ip_be);

server_client_t *server_find_by_ip(uint32_t ip_be)
{
    size_t i;

    /* The client whose current address it is, else one that holds it as
     * a pending or previous address (#77) */
    if (idx_ok) {
        long k = slotidx_find(&idx_ip, slotidx_hash_u32(ip_be), match_cur_ip,
                              &ip_be);

        if (k < 0) {
            k = slotidx_find(&idx_ip, slotidx_hash_u32(ip_be), match_ip,
                             &ip_be);
        }
        return k >= 0 ? &server_clients[k] : NULL;
    }
    for (i = 0; i < server_max_clients; ++i) {
        if (match_cur_ip((uint32_t)i, &ip_be)) {
            return &server_clients[i];
        }
    }
    for (i = 0; i < server_max_clients; ++i) {
        if (match_ip((uint32_t)i, &ip_be)) {
            return &server_clients[i];
        }
    }

    return NULL;
}

server_client_t *server_find_by_client_id(const char *client_id)
{
    size_t i;

    if (client_id == NULL || *client_id == '\0') {
        return NULL;
    }
    if (idx_ok) {
        long k = slotidx_find(&idx_id, slotidx_hash_nocase(client_id),
                              match_id, client_id);

        return k >= 0 ? &server_clients[k] : NULL;
    }
    for (i = 0; i < server_max_clients; ++i) {
        if (server_clients[i].active &&
            strcasecmp(server_clients[i].client_id, client_id) == 0) {
            return &server_clients[i];
        }
    }

    return NULL;
}

server_client_t *server_find_by_addr(const struct sockaddr_in *addr)
{
    size_t i;

    if (addr == NULL) {
        return NULL;
    }
    if (idx_ok && addr_is_set(addr)) {
        long k = slotidx_find(&idx_addr, hash_addr(addr), match_addr, addr);

        return k >= 0 ? &server_clients[k] : NULL;
    }
    for (i = 0; i < server_max_clients; ++i) {
        if (server_clients[i].active &&
            net_sockaddr_equal(&server_clients[i].addr, addr)) {
            return &server_clients[i];
        }
    }

    return NULL;
}

server_client_t *server_alloc_client(void)
{
    size_t n;
    size_t i;
    size_t oldest = 0;
    time_t oldest_seen = 0;
    int oldest_found = 0;

    if (server_max_clients == 0U) {
        return NULL;
    }
    for (n = 0; n < server_max_clients; ++n) {
        i = (alloc_next + n) % server_max_clients;
        if (!server_clients[i].active) {
            memset(&server_clients[i], 0, sizeof(server_clients[i]));
            server_clients[i].tcp_fd = -1;
            server_clients[i].active = 1U;
            alloc_next = (i + 1U) % server_max_clients;
            return &server_clients[i];
        }
    }

    /* Just started: the clients now coming back must not be evicted for
     * new ones that get no address yet (#81) */
    if (server_pool_holding()) {
        return NULL;
    }
    /* Full: evict the client that was quiet longest */
    for (i = 0; i < server_max_clients; ++i) {
        if (!oldest_found || server_clients[i].last_seen < oldest_seen) {
            oldest_found = 1;
            oldest = i;
            oldest_seen = server_clients[i].last_seen;
        }
    }
    server_end_client(&server_clients[oldest]);
    server_clients[oldest].active = 1U;
    return &server_clients[oldest];
}

/* An entry ends while its client may still be alive: an auto client's
 * address stays leased to its ID (#81) */
void server_end_client(server_client_t *entry)
{
    if (entry == NULL) {
        return;
    }
    if (entry->active && entry->registered && entry->auto_client &&
        entry->client_id[0] != '\0') {
        if (pool_has(entry->ip_be) &&
            profile_owner_of_ip(entry->ip_be) == NULL) {
            server_lease_add(entry->client_id, entry->ip_be, 0);
        }
        /* Its previous session may still send from the old address */
        if (entry->prev.valid && entry->prev_ip_be != entry->ip_be &&
            pool_has(entry->prev_ip_be) &&
            profile_owner_of_ip(entry->prev_ip_be) == NULL) {
            server_lease_add(entry->client_id, entry->prev_ip_be, 1);
        }
    }
    server_drop_entry(entry);
}

void server_drop_entry(server_client_t *entry)
{
    if (entry == NULL) {
        return;
    }
    if (entry->tcp_fd >= 0) {
        net_tcp_close(entry->tcp_fd);
    }
    crypto_wipe(entry, sizeof(*entry));
    entry->tcp_fd = -1;
}

/* Seconds of silence before a client expires: CAMEX_CLIENT_TIMEOUT; the
 * environment variable CAMEX_CLIENT_TIMEOUT (5..120) shortens it, for
 * tests */
static double server_client_timeout(void)
{
    static int value = 0;

    if (value == 0) {
        const char *env = getenv("CAMEX_CLIENT_TIMEOUT");
        int v = env != NULL ? atoi(env) : 0;

        value = (v >= 5 && v <= CAMEX_CLIENT_TIMEOUT) ? v
                                                      : CAMEX_CLIENT_TIMEOUT;
    }
    return (double)value;
}

void server_expire_clients(void)
{
    static time_t last_expire_check = 0;
    size_t i;
    time_t now = (g_now != 0) ? g_now : time(NULL);

    if (now == last_expire_check) {
        return;
    }
    last_expire_check = now;

    for (i = 0; i < server_max_clients; ++i) {
        server_client_t *c = &server_clients[i];
        int preauth = c->tcp_fd >= 0 && !c->registered;

        if (!c->active) {
            continue;
        }
        if (c->prev.valid && now > c->prev_until) {
            session_wipe(&c->prev);
            c->prev_ip_be = 0U;   /* its address is free again (#77) */
        }
        /* A handshake the client never confirmed */
        if (c->next.valid &&
            difftime(now, c->next.created) > (double)V2_PREV_SESSION_GRACE) {
            session_wipe(&c->next);
            if (c->pending_ip_be != 0U) {
                char ip[16];

                ip_text(ntohl(c->pending_ip_be), ip, sizeof(ip));
                log_message(LOG_INFO, "Client %s did not confirm %s: it "
                            "keeps its address", c->client_id, ip);
                c->pending_ip_be = 0U;   /* the old address stays (#77) */
            }
        }
        /* Pre-auth: counted from the accept, so frames that arrive without
         * a handshake cannot keep an unregistered connection open. */
        if (preauth ? difftime(now, c->connected_at) >
                          (double)CAMEX_TCP_PREAUTH_TIMEOUT
                    : difftime(now, c->last_seen) >
                          server_client_timeout()) {
            char ipbuf[INET_ADDRSTRLEN];
            struct in_addr addr;

            addr.s_addr = c->ip_be;
            inet_ntop(AF_INET, &addr, ipbuf, sizeof(ipbuf));
            if (preauth) {
                char peer[64];

                net_sockaddr_to_string(&c->addr, peer, sizeof(peer));
                log_peer(LOG_INFO,
                         "Closed TCP connection from %s: no handshake "
                         "within %ds", peer, CAMEX_TCP_PREAUTH_TIMEOUT);
            } else {
                log_message(LOG_INFO, "Expired client %s", ipbuf);
            }
            server_end_client(c);
        }
    }
}

/* May entry (client_id) have ip_be from the auto pool? In the pool, not
 * the server's tunnel address nor a [client ID] profile's, not held by
 * another live client (current, pending or previous address) and not
 * leased to another ID (#15, #75, #81) */
static int server_ip_usable(const server_client_t *entry, uint32_t ip_be,
                            const char *client_id)
{
    const server_client_t *user;

    if (!pool_has(ip_be) || ip_be == server_tunnel_ip ||
        profile_owner_of_ip(ip_be) != NULL) {
        return 0;
    }
    user = server_find_by_ip(ip_be);
    return (user == NULL || user == entry) &&
           server_lease_allows(ip_be, client_id);
}

/* Take a free auto-pool address for entry (#15, #75): one that
 * server_ip_usable() allows, from a cursor that walks the pool. When there
 * is none, the address of the oldest lease past SERVER_LEASE_MIN (#81).
 * Returns 0 and the address (network order), or -1 if the pool is full. A
 * full pool is walked at most once a second: clients expire on whole
 * seconds, and a client that finds no address retries. */
static int server_auto_pool_take(const server_client_t *entry,
                                 uint32_t *ip_be)
{
    const uint32_t first = pool_first();
    const uint32_t size = pool_last() - first + 1U;
    const uint32_t start = (auto_pool_next >= first &&
                            auto_pool_next <= pool_last())
                               ? auto_pool_next - first : 0U;
    server_lease_t *oldest = NULL;
    uint32_t n;

    if (pool_full_at == g_now && pool_full_at != 0 &&
        pool_full_net == server_db.globals.pool_net &&
        pool_full_prefix == server_db.globals.pool_prefix) {
        return -1;
    }
    for (n = 0U; n < size; ++n) {
        uint32_t host = first + (start + n) % size;
        uint32_t a = htonl(host);
        server_lease_t *l;
        const server_client_t *user;

        if (server_ip_usable(entry, a, entry->client_id)) {
            *ip_be = a;
            auto_pool_next = host < pool_last() ? host + 1U : first;
            return 0;
        }
        /* Leased to another ID and otherwise free: a candidate for when
         * the pool has nothing else */
        l = server_lease_by_ip(a);
        if (l != NULL && a != server_tunnel_ip &&
            profile_owner_of_ip(a) == NULL &&
            ((user = server_find_by_ip(a)) == NULL || user == entry) &&
            difftime(g_now, l->since) >= (double)SERVER_LEASE_MIN &&
            (oldest == NULL || l->since < oldest->since)) {
            oldest = l;
        }
    }
    if (oldest != NULL) {
        char ip[16];

        ip_text(ntohl(oldest->ip_be), ip, sizeof(ip));
        log_message(LOG_NOTICE, "Auto pool full: %s of client %s (gone for "
                    "%.0f s) goes to client %s", ip, oldest->id,
                    difftime(g_now, oldest->since), entry->client_id);
        /* The lease goes when the handshake succeeds */
        *ip_be = oldest->ip_be;
        return 0;
    }
    pool_full_at = g_now;
    pool_full_net = server_db.globals.pool_net;
    pool_full_prefix = server_db.globals.pool_prefix;
    return -1;
}

void server_revalidate_unknown_clients(void)
{
    size_t i;

    if (!server_db.globals.deny_unknown_clients) {
        return;
    }
    /* After a reload that set allow_unknown_clients = no, auto clients
     * whose ID is not in the file lose their session (#15). */
    for (i = 0U; i < server_max_clients; ++i) {
        server_client_t *c = &server_clients[i];

        if (!c->active || !c->auto_client ||
            config_db_find_client(c->client_id) != NULL) {
            continue;
        }
        log_message(LOG_NOTICE,
                    "Ended session of client %s: unknown client ID "
                    "(allow_unknown_clients = no)", c->client_id);
        /* It may still run: its address stays kept (#81) */
        server_end_client(c);
    }
}

void server_revalidate_profile_addresses(void)
{
    size_t i;

    /* A reload may give a live client's address to another client's
     * [client ID] local_cidr: that client's session ends, so the profile's
     * client can have the address and the other one gets a new one when
     * it comes back (#81). A manual client without an ID may be the
     * profile's own device: it stays, and the profile's client is refused
     * while it has the address. */
    for (i = 0U; i < server_max_clients; ++i) {
        server_client_t *c = &server_clients[i];
        const char *owner;

        if (!c->active || !c->registered) {
            continue;
        }
        if (c->pending_ip_be != 0U &&
            (owner = profile_owner_of_ip(c->pending_ip_be)) != NULL &&
            strcasecmp(owner, c->client_id) != 0) {
            /* The offer is withdrawn, and the unconfirmed session whose
             * CONFIG carried it ends: the client must not confirm it */
            c->pending_ip_be = 0U;
            session_wipe(&c->next);
        }
        owner = profile_owner_of_ip(c->ip_be);
        if (owner == NULL || strcasecmp(owner, c->client_id) == 0 ||
            (!c->auto_client && c->client_id[0] == '\0')) {
            continue;
        }
        {
            char ip[16];

            ip_text(ntohl(c->ip_be), ip, sizeof(ip));
            log_message(LOG_NOTICE,
                        "Ended session of client %s: its address %s is now "
                        "client %s's (config reload)",
                        c->client_id[0] != '\0' ? c->client_id : "(manual)",
                        ip, owner);
        }
        server_drop_entry(c);
    }
}

/* CONFIG for an auto client (the handshake response payload); takes an
 * auto-pool address when the profile has none (#15), preferring hint_ip
 * (the client's previous address) when it is free. */
static int server_build_config(server_client_t *entry, const char *client_id,
                               const char *hint_ip,
                               char *message, size_t message_size)
{
    camex_profile_t profile;
    uint32_t old_ip_be = entry == NULL ? 0U
                         : entry->pending_ip_be != 0U ? entry->pending_ip_be
                                                      : entry->ip_be;
    /* An offer this handshake may withdraw (#77) */
    uint32_t offered_ip_be = entry == NULL ? 0U : entry->pending_ip_be;

    if (config_db_lookup_profile(client_id, &profile) != 0) {
        log_message(LOG_ERR, "Missing config for client %s",
                    client_id != NULL ? client_id : "(null)");
        return -1;
    }

    if (profile.local_cidr[0] == '\0' || profile.gateway_ip[0] == '\0') {
        /* No config file loaded — auto-assign dynamic IP for auto-config client */
        if (entry != NULL && client_id != NULL && client_id[0] != '\0') {
            uint32_t ip_be = 0U;
            char ip[16];

            /* A new entry: the address leased to its ID (#81), else the
             * one the client asks for (its previous one) when it is free */
            if (entry->ip_be == 0U) {
                const server_lease_t *l = server_lease_by_id(client_id);
                struct in_addr h;

                if (l != NULL &&
                    server_ip_usable(entry, l->ip_be, client_id)) {
                    server_set_ip(entry, l->ip_be);
                } else if (hint_ip != NULL && hint_ip[0] != '\0' &&
                           inet_pton(AF_INET, hint_ip, &h) == 1 &&
                           server_ip_usable(entry, h.s_addr, client_id)) {
                    server_set_ip(entry, h.s_addr);
                }
            }
            /* The address the client was last given: a pending one when
             * it has not confirmed it yet (#77) */
            uint32_t given = entry->pending_ip_be != 0U ? entry->pending_ip_be
                                                        : entry->ip_be;

            if (given != 0U && server_ip_usable(entry, given, client_id)) {
                /* Keep the same IP (a changed pool takes it away) */
                ip_be = given;
            } else if (given != entry->ip_be && entry->ip_be != 0U &&
                       server_ip_usable(entry, entry->ip_be, client_id)) {
                /* The pending address is not usable any more, the current
                 * one still is: keep it (#77) */
                ip_be = entry->ip_be;
            } else if (server_pool_holding()) {
                /* Just started: the addresses of clients that have not
                 * come back yet are unknown (#81); the client retries */
                log_peer(LOG_NOTICE, "Auto pool: no new address for client "
                         "%s yet, the server waits for returning clients "
                         "after its start", client_id);
                return -1;
            } else if (server_auto_pool_take(entry, &ip_be) != 0) {
                char net[16];

                ip_text(server_db.globals.pool_net, net, sizeof(net));
                log_peer(LOG_WARNING,
                         "Auto pool %s/%u is full: no address for client %s",
                         net, (unsigned)server_db.globals.pool_prefix,
                         client_id);
                return -1;
            }
            ip_text(ntohl(ip_be), ip, sizeof(ip));
            snprintf(profile.local_cidr, sizeof(profile.local_cidr),
                     "%s/%u", ip, (unsigned)server_db.globals.pool_prefix);
            ip_text(pool_gateway(), profile.gateway_ip,
                    sizeof(profile.gateway_ip));
            uint32_t was_ip_be = entry->ip_be;

            /* Store assigned IP in client entry for packet forwarding;
             * leases go once the reply is out (server_handle_init()), so
             * a failed handshake keeps them (#81) */
            server_assign_ip(entry, ip_be);
            /* Rekeys repeat the CONFIG: log new assignments only. A live
             * client's new address is only pending until it confirms the
             * new session (#77): server_handle_data() logs the move */
            if (offered_ip_be != 0U && ip_be != offered_ip_be &&
                ip_be == was_ip_be) {
                log_message(LOG_INFO,
                            "Client %s keeps %s: the address it was "
                            "offered is no longer usable",
                            client_id, profile.local_cidr);
            } else if (ip_be != old_ip_be && entry->pending_ip_be == ip_be) {
                log_message(LOG_INFO,
                            "Offered %s to client %s (it moves there when "
                            "it confirms the new session)",
                            profile.local_cidr, client_id);
            } else if (ip_be != old_ip_be) {
                log_message(LOG_INFO,
                            "Auto-assigned %s to client %s (no config file)",
                            profile.local_cidr, client_id);
            }
        } else {
            log_message(LOG_ERR, "Incomplete config for client %s",
                        client_id != NULL ? client_id : "(null)");
            return -1;
        }
    }

    return proto_build_config(client_id, &profile, message, message_size);
}

/*
 * Key ownership (#5). The keystore holds the global key (owner "") and
 * one entry per [client ID] that has its own psk=. A client that has its
 * own key may only use that key; the global key is for clients that have
 * none.
 */
static const char *keystore_owner(const uint8_t *key)
{
    return config_ks_owner(key);
}

static int keystore_has_owner(const char *client_id)
{
    return config_ks_has_owner(client_id);
}

static int keystore_key_owned_by(const uint8_t *key, const char *client_id)
{
    return config_ks_key_owned_by(key, client_id);
}

/* Tunnel IP configured for client_id in its own [client ID] section. */
static int profile_ip(const char *client_id, uint32_t *ip_be)
{
    camex_server_profile_t *p = config_db_find_client(client_id);

    if (p == NULL || !p->has_ip) {
        return -1;
    }
    *ip_be = p->ip_be;
    return 0;
}

/* The [client ID] section whose local_cidr address is ip_be, if any. */
static const char *profile_owner_of_ip(uint32_t ip_be)
{
    camex_server_profile_t *p = config_db_find_profile_by_ip(ip_be);

    return p != NULL ? p->client_id : NULL;
}

static int keystore_contains(const uint8_t *key)
{
    return config_ks_contains(key);
}

static int keystore_is_global(const uint8_t *key)
{
    return config_ks_is_global(key);
}

void server_revalidate_keys(void)
{
    size_t i;

    /* After a config reload (SIGHUP) every live session must still be
     * allowed with the key it registered with: a removed or changed key,
     * or a key that now belongs to another client, ends it (#16 L3). */
    for (i = 0U; i < server_max_clients; ++i) {
        server_client_t *c = &server_clients[i];
        const char *owner;
        int allowed;

        if (!c->active || !c->registered) {
            continue;
        }
        owner = keystore_owner(c->psk_key);
        /* A session without a client ID registered with the global key
         * (a per-client key would have named its owner): it must still be
         * the global key, not bytes a profile now owns. */
        allowed = keystore_contains(c->psk_key) &&
                  (c->client_id[0] == '\0'
                       ? keystore_is_global(c->psk_key)
                       : (keystore_key_owned_by(c->psk_key, c->client_id) ||
                          (owner[0] == '\0' &&
                           !keystore_has_owner(c->client_id))));
        if (allowed) {
            continue;
        }
        log_message(LOG_NOTICE,
                    "Ended session of client %s: its key is no longer "
                    "allowed after the config reload",
                    c->client_id[0] != '\0' ? c->client_id : "(manual)");
        /* It may still run: its address stays kept (#81) */
        server_end_client(c);
    }
}

/*
 * May a handshake authenticated with key claim client_id / local_ip?
 * For a manual REGISTER without CLIENT_ID the owner of a per-client key
 * becomes the client ID. Returns 0 if allowed, -1 if rejected.
 */
static int server_register_allowed(const uint8_t *key, char *client_id,
                                   size_t client_id_size,
                                   const char *local_ip, uint8_t auto_request,
                                   const struct sockaddr_in *from)
{
    char peer[64];
    const char *owner = keystore_owner(key);
    int global_key = (owner[0] == '\0');

    net_sockaddr_to_string(from, peer, sizeof(peer));

    if (client_id[0] == '\0' && !global_key) {
        snprintf(client_id, client_id_size, "%s", owner);
    }
    /* The named client's own key always counts as its key, even when the
     * same bytes are also the global key or another client's key
     * (keystore_owner() only reports the first match). */
    if (client_id[0] != '\0' &&
        !keystore_key_owned_by(key, client_id) &&
        (!global_key || keystore_has_owner(client_id))) {
        log_peer(LOG_WARNING,
                 "Rejected handshake from %s: key does not belong "
                 "to client %s", peer, client_id);
        return -1;
    }

    if (!auto_request) {
        struct in_addr claimed;
        uint32_t want;
        const char *ip_owner;

        if (inet_pton(AF_INET, local_ip, &claimed) != 1) {
            return -1;
        }
        if (client_id[0] != '\0' && profile_ip(client_id, &want) == 0 &&
            want != claimed.s_addr) {
            log_peer(LOG_WARNING,
                     "Rejected handshake from %s: client %s claims %s, "
                     "configured address differs", peer, client_id,
                     local_ip);
            return -1;
        }
        ip_owner = profile_owner_of_ip(claimed.s_addr);
        if (ip_owner != NULL &&
            strcasecmp(ip_owner, client_id) != 0 &&
            keystore_has_owner(ip_owner)) {
            log_peer(LOG_WARNING,
                     "Rejected handshake from %s: address %s belongs "
                     "to client %s", peer, local_ip, ip_owner);
            return -1;
        }
    }

    return 0;
}

/*
 * Handshake budgets (#9, #16, #75) and their fair shares (#78). Every INIT
 * that passes mac1 costs X25519 operations and key trials, and every
 * client knows the server's public key, so the per-second budgets are
 * shared out by source:
 *  - a known source (the address and port of a client that sent
 *    authenticated data within CAMEX_CLIENT_TIMEOUT, as a rekey does) has
 *    its own key tried first; that trial and up to SERVER_KNOWN_PER_SEC of
 *    its INITs a second count only against the whole budgets. Anything
 *    more (further INITs, other keys) counts as from an unknown source, so
 *    a client, or a sender that forges its address, cannot drain them;
 *  - unknown sources together use at most SERVER_UNKNOWN_SHARE of each, so
 *    a flood from unknown sources does not stop live clients' rekeys;
 *  - one source address at most SERVER_SRC_SHARE, one /24 at most
 *    SERVER_NET_SHARE of each; the key trial shares are at least one and
 *    two keystores' worth, so a handshake can always reach every key.
 * Sources of the current second sit in a small table, placed by a keyed
 * hash, so a sender cannot choose addresses that crowd out another one;
 * a source that finds no free slot (hundreds of sources in one second) is
 * held by the unknown sources' share only.
 */
#define SERVER_UNKNOWN_SHARE(n) ((n) / 4U * 3U)
#define SERVER_SRC_SHARE(n)     ((n) / 8U)
#define SERVER_NET_SHARE(n)     ((n) / 4U)
#define SERVER_KNOWN_PER_SEC 4U
#define SERVER_SRC_SLOTS  512U
#define SERVER_SRC_PROBES 8U

typedef struct {
    uint32_t key;          /* source address or its /24, host order */
    uint8_t is_net;
    time_t window;         /* second the counts belong to */
    unsigned int handshakes;
    size_t trials;
} server_share_t;

typedef struct {
    uint32_t a;            /* source address, host order */
    int looked_up;         /* addr and net are set (server_source_shares()) */
    server_share_t *addr;  /* NULL: no slot this second */
    server_share_t *net;
    server_client_t *known;   /* the live client at this source, or NULL */
} server_source_t;

static server_share_t server_shares[SERVER_SRC_SLOTS];
static uint8_t server_share_key[16];
static int server_share_key_ready = 0;
static time_t budget_window = 0;
static unsigned int budget_handshakes = 0U;   /* all sources */
static unsigned int budget_unknown_handshakes = 0U;
static size_t budget_trials = 0U;
static size_t budget_unknown_trials = 0U;

static void server_budget_window(void)
{
    if (budget_window != g_now) {
        budget_window = g_now;
        budget_handshakes = 0U;
        budget_unknown_handshakes = 0U;
        budget_trials = 0U;
        budget_unknown_trials = 0U;
    }
}

/* The share entry of key for this second, or NULL when its slots are
 * taken */
static server_share_t *server_share_of(uint32_t key, uint8_t is_net)
{
    uint8_t msg[5];
    uint8_t hash[4];
    uint32_t h;
    server_share_t *free_slot = NULL;
    server_share_t *s;
    uint32_t p;

    if (!server_share_key_ready) {
        /* Without randomness the placement is only predictable: the key
         * stays as it is and the next INIT tries again */
        uint8_t fresh[sizeof(server_share_key)];

        if (get_random_bytes(fresh, sizeof(fresh)) == 0) {
            memcpy(server_share_key, fresh, sizeof(fresh));
            server_share_key_ready = 1;
        }
        crypto_wipe(fresh, sizeof(fresh));
    }
    msg[0] = (uint8_t)(key >> 24);
    msg[1] = (uint8_t)(key >> 16);
    msg[2] = (uint8_t)(key >> 8);
    msg[3] = (uint8_t)key;
    msg[4] = is_net;
    crypto_blake2b_keyed(hash, sizeof(hash), server_share_key,
                         sizeof(server_share_key), msg, sizeof(msg));
    h = ((uint32_t)hash[0] << 24) | ((uint32_t)hash[1] << 16) |
        ((uint32_t)hash[2] << 8) | (uint32_t)hash[3];
    for (p = 0U; p < SERVER_SRC_PROBES; ++p) {
        s = &server_shares[(h + p) % SERVER_SRC_SLOTS];
        if (s->window != g_now) {
            if (free_slot == NULL) {
                free_slot = s;
            }
        } else if (s->key == key && s->is_net == is_net) {
            return s;
        }
    }
    if (free_slot != NULL) {
        free_slot->key = key;
        free_slot->is_net = is_net;
        free_slot->window = g_now;
        free_slot->handshakes = 0U;
        free_slot->trials = 0U;
    }
    return free_slot;
}

/* The live client at from, or NULL: its handshakes are rekeys */
static server_client_t *server_known_source(const struct sockaddr_in *from)
{
    server_client_t *c = server_find_by_addr(from);

    if (c == NULL || !c->registered || !c->cur.valid ||
        c->last_data == 0 || g_now - c->last_data > CAMEX_CLIENT_TIMEOUT) {
        return NULL;
    }
    /* SERVER_KNOWN_PER_SEC INITs a second; more count as unknown */
    if (c->hs_window != g_now) {
        c->hs_window = g_now;
        c->hs_count = 0U;
    }
    if (c->hs_count >= SERVER_KNOWN_PER_SEC) {
        return NULL;
    }
    c->hs_count++;
    return c;
}

static void server_source_for(const struct sockaddr_in *from,
                              server_source_t *src)
{
    server_budget_window();
    src->a = ntohl(from->sin_addr.s_addr);
    src->looked_up = 0;
    src->addr = NULL;
    src->net = NULL;
    src->known = server_known_source(from);
}

/* The source's share entries, looked up only once the whole budgets
 * leave room: a flood over them costs no hashing */
static void server_source_shares(server_source_t *src)
{
    if (!src->looked_up) {
        src->looked_up = 1;
        src->addr = server_share_of(src->a, 0U);
        src->net = server_share_of(src->a & 0xFFFFFF00U, 1U);
    }
}

/* One more INIT within the budgets: 0, -1 when all handshakes of this
 * second are taken, -2 when those of unknown sources are, -3 when this
 * source's or its /24's share is */
static int server_handshake_budget(server_source_t *src)
{
    if (budget_handshakes >= SERVER_HANDSHAKES_PER_SEC) {
        return -1;
    }
    if (src->known == NULL) {
        if (budget_unknown_handshakes >=
            SERVER_UNKNOWN_SHARE(SERVER_HANDSHAKES_PER_SEC)) {
            return -2;
        }
        server_source_shares(src);
        if ((src->addr != NULL &&
             src->addr->handshakes >=
                 SERVER_SRC_SHARE(SERVER_HANDSHAKES_PER_SEC)) ||
            (src->net != NULL &&
             src->net->handshakes >=
                 SERVER_NET_SHARE(SERVER_HANDSHAKES_PER_SEC))) {
            return -3;
        }
        if (src->addr != NULL) {
            src->addr->handshakes++;
        }
        if (src->net != NULL) {
            src->net->handshakes++;
        }
        budget_unknown_handshakes++;
    }
    budget_handshakes++;
    return 0;
}

/* Key trials allowed per second: SERVER_KEY_TRIALS_PER_SEC, or four times
 * the keystore when it is larger (one handshake may have to try every
 * key, and unknown sources must not take the whole budget with one or
 * two) */
static size_t server_key_trials_limit(void)
{
    size_t limit = SERVER_KEY_TRIALS_PER_SEC;

    if (server_keystore_count * 4U > limit) {
        limit = server_keystore_count * 4U;
    }
    return limit;
}

static size_t server_trial_share(int is_net)
{
    size_t limit = server_key_trials_limit();
    size_t share = is_net ? SERVER_NET_SHARE(limit) : SERVER_SRC_SHARE(limit);
    size_t floor = server_keystore_count * (is_net ? 2U : 1U);

    return share > floor ? share : floor;
}

/* One more key trial within the budgets, as server_handshake_budget();
 * own: the trial of a known source's own key */
static int server_key_trial_budget(server_source_t *src, int own)
{
    size_t limit = server_key_trials_limit();

    if (budget_trials >= limit) {
        return -1;
    }
    if (!own) {
        if (budget_unknown_trials >= SERVER_UNKNOWN_SHARE(limit)) {
            return -2;
        }
        server_source_shares(src);
        if ((src->addr != NULL && src->addr->trials >= server_trial_share(0)) ||
            (src->net != NULL && src->net->trials >= server_trial_share(1))) {
            return -3;
        }
        if (src->addr != NULL) {
            src->addr->trials++;
        }
        if (src->net != NULL) {
            src->net->trials++;
        }
        budget_unknown_trials++;
    }
    budget_trials++;
    return 0;
}

static int server_send(server_client_t *entry, const uint8_t *pkt, size_t len)
{
    if (entry->tcp_fd >= 0) {
        return net_tcp_send_frame(entry->tcp_fd, pkt, len);
    }
    if (current_config.transport == CAMEX_TRANSPORT_TCP) {
        return -1;
    }
    return net_send_payload(net_fd, &entry->addr, pkt, len);
}

/* A session index for entry, or 0 (#75): the low 16 bits are the slot
 * number + 1, so a data packet finds its client without a search; the high
 * 16 bits are random and differ from the entry's other sessions. The slot
 * number is readable on the wire, as the whole index always was. */
static uint32_t server_new_index(const server_client_t *entry)
{
    uint32_t slot1 = (uint32_t)(entry - server_clients) + 1U;
    int tries;

    for (tries = 0; tries < 16; ++tries) {
        uint32_t idx = v2_new_index();

        if (idx == 0U) {
            return 0U;
        }
        idx = (idx & 0xFFFF0000U) | slot1;
        if (!((entry->cur.valid && entry->cur.local_index == idx) ||
              (entry->next.valid && entry->next.local_index == idx) ||
              (entry->prev.valid && entry->prev.local_index == idx))) {
            return idx;
        }
    }
    return 0U;
}

/* A client that came back on a new TCP connection: the entry takes the new
 * fd, the slot that connection got at accept() is freed, the old
 * connection is closed */
static void server_adopt_tcp(server_client_t *entry, int fd)
{
    size_t j;

    if (fd < 0 || entry->tcp_fd == fd) {
        return;
    }
    for (j = 0; j < server_max_clients; ++j) {
        if (server_clients[j].active && server_clients[j].tcp_fd == fd &&
            &server_clients[j] != entry) {
            crypto_wipe(&server_clients[j], sizeof(server_clients[j]));
            server_clients[j].tcp_fd = -1;
        }
    }
    if (entry->tcp_fd >= 0) {
        net_tcp_close(entry->tcp_fd);
    }
    entry->tcp_fd = fd;
    memset(&entry->recv_state, 0, sizeof(entry->recv_state));
}

/* The client entry a handshake belongs to (created or reused), or NULL if
 * it must be rejected. *fresh is set when a new slot was taken. */
static server_client_t *server_entry_for(const uint8_t *key,
                                         const char *client_id,
                                         const char *local_ip,
                                         uint8_t auto_request,
                                         const struct sockaddr_in *from,
                                         int fd, int *fresh)
{
    server_client_t *entry;
    char peer[64];

    *fresh = 0;
    net_sockaddr_to_string(from, peer, sizeof(peer));

    if (auto_request) {
        /* allow_unknown_clients = no: only IDs with a [client ID] section
         * get a configuration (#15). */
        if (server_db.globals.deny_unknown_clients &&
            config_db_find_client(client_id) == NULL) {
            log_peer(LOG_WARNING,
                     "Rejected handshake from %s: unknown client ID %s "
                     "(allow_unknown_clients = no)", peer, client_id);
            return NULL;
        }
        entry = server_find_by_client_id(client_id);
        if (entry != NULL) {
            if (!net_sockaddr_equal(&entry->addr, from)) {
                char old_peer[64];

                net_sockaddr_to_string(&entry->addr, old_peer,
                                       sizeof(old_peer));
                log_message(LOG_INFO, "Client %s reconnected: %s -> %s",
                            client_id, old_peer, peer);
            }
            server_adopt_tcp(entry, fd);
            return entry;
        }
        /* TCP clients already have a slot allocated at accept() */
        entry = server_find_by_addr(from);
        if (entry != NULL && entry->tcp_fd >= 0) {
            return entry;
        }
        entry = server_alloc_client();
        if (entry != NULL) {
            *fresh = 1;
        }
        return entry;
    }

    {
        struct in_addr addr;
        server_client_t *by_addr = server_find_by_addr(from);
        server_client_t *by_ip;

        if (inet_pton(AF_INET, local_ip, &addr) != 1) {
            log_peer(LOG_WARNING, "Rejected invalid client IP: %s", local_ip);
            return NULL;
        }
        by_ip = server_find_by_ip(addr.s_addr);
        /* An address that is a client's pending or previous one (#77) is
         * reserved: a manual handshake cannot claim it, the client it is
         * reserved for included (taking it at once would move that
         * client's address before it confirms). Once the offer is
         * confirmed or expires, at most V2_PREV_SESSION_GRACE later, it
         * can. */
        if (by_ip != NULL && by_ip->ip_be != addr.s_addr) {
            log_peer(LOG_WARNING,
                     "Rejected handshake from %s: address %s is reserved "
                     "for client %s", peer, local_ip,
                     by_ip->client_id[0] != '\0' ? by_ip->client_id
                                                  : "(manual)");
            return NULL;
        }
        /* A live client has the address: only the same manual client may
         * take its entry over (a restart, a new port or TCP connection):
         * same ID or none, same key. Never an auto client's (#81), whose
         * address came from the pool, nor another client's. */
        if (by_ip != NULL &&
            (by_ip->auto_client ||
             (by_ip != by_addr &&
              (strcasecmp(by_ip->client_id, client_id) != 0 ||
               key == NULL || memcmp(by_ip->psk_key, key, 32) != 0)))) {
            log_peer(LOG_WARNING,
                     "Rejected handshake from %s: address %s is in use "
                     "by client %s", peer, local_ip,
                     by_ip->client_id[0] != '\0' ? by_ip->client_id
                                                  : "(manual)");
            return NULL;
        }
        /* An auto client that is gone keeps its address for a while
         * (#81): it may still be using it */
        {
            const server_lease_t *l = server_lease_by_ip(addr.s_addr);

            if (l != NULL && strcasecmp(l->id, client_id) != 0 &&
                difftime(g_now, l->since) < (double)SERVER_LEASE_MIN) {
                log_peer(LOG_WARNING,
                         "Rejected handshake from %s: address %s is kept "
                         "for client %s (gone for %.0f s)", peer, local_ip,
                         l->id, difftime(g_now, l->since));
                return NULL;
            }
        }
        if (by_ip != NULL && (by_addr == NULL || by_addr == by_ip ||
                              (by_addr->tcp_fd >= 0 && !by_addr->registered))) {
            /* The client's own entry, also when it came back on a new TCP
             * connection (whose pre-auth slot by_addr is then freed) */
            if (!net_sockaddr_equal(&by_ip->addr, from)) {
                char old_peer[64];

                net_sockaddr_to_string(&by_ip->addr, old_peer,
                                       sizeof(old_peer));
                log_message(LOG_INFO, "Client %s reconnected: %s -> %s",
                            local_ip, old_peer, peer);
            }
            server_adopt_tcp(by_ip, fd);
            entry = by_ip;
        } else if (by_addr != NULL && (fd >= 0 || !by_addr->cur.valid)) {
            /* The same connection or address, without a working session:
             * a client that changed its address. A UDP entry that has one
             * is not taken over by a handshake for another address (a
             * forged source plus a replayed message 1 could otherwise cut
             * that client off): such a handshake gets a new entry. */
            entry = by_addr;
        } else {
            entry = server_alloc_client();
            if (entry == NULL) {
                return NULL;
            }
            *fresh = 1;
        }
        server_set_ip(entry, addr.s_addr);
        return entry;
    }
}

/* INIT: Noise message 1 with the client's REGISTER as payload */
static int server_handle_init(const uint8_t *buffer, size_t len,
                              const struct sockaddr_in *from, int fd)
{
    const camex_crypto_t *ctx = crypto_get_ctx();
    noise_hs_t base;
    noise_hs_t hs;
    uint8_t es[NOISE_DHLEN];
    uint8_t payload[V2_TIMESTAMP_LEN + CAMEX_CONTROL_MAX];
    size_t payload_len = 0U;
    const uint8_t *used_key = NULL;
    char message[CAMEX_CONTROL_MAX];
    char reply[CAMEX_CONTROL_MAX];
    char client_id[CAMEX_CLIENT_ID_LEN];
    char local_ip[16];
    uint8_t auto_request = 0U;
    uint8_t e_seed[NOISE_DHLEN];
    uint8_t resp[V2_RESP_HDR + NOISE_MSG2_OVERHEAD + CAMEX_CONTROL_MAX];
    uint8_t c1[NOISE_KEYLEN];
    uint8_t c2[NOISE_KEYLEN];
    uint32_t client_index = v2_read_u32(buffer + 5);
    uint32_t server_index;
    server_client_t *entry;
    char peer[64];
    size_t resp_len;
    size_t i;
    size_t n;
    size_t ahead;
    long own;
    server_source_t src;
    int budget;
    int fresh = 0;
    uint32_t pending_before = 0U;
    uint32_t ip_before = 0U;
    session_t next_before;
    int next_saved = 0;
    int cur_set = 0;
    uint8_t psk_before[32];
    int first;

    net_sockaddr_to_string(from, peer, sizeof(peer));
    if (!ctx->has_server_key) {
        return -1;
    }
    /* mac1 first: packets from anyone who does not know the server's
     * public key cost one hash, and do not count against the budget */
    {
        uint8_t mac[V2_MAC_LEN];

        len -= V2_MAC_LEN;
        v2_mac1(ctx->server_pub, buffer, len, mac);
        if (crypto_verify16(mac, buffer + len) != 0) {
            log_peer(LOG_WARNING, "Dropped handshake from %s: bad mac1 "
                     "(wrong server public key?)", peer);
            return -1;
        }
    }
    server_source_for(from, &src);
    budget = server_handshake_budget(&src);
    if (budget == -1) {
        log_peer(LOG_WARNING, "Dropped handshake from %s: more than %u "
                 "handshakes per second", peer, SERVER_HANDSHAKES_PER_SEC);
        return -1;
    }
    if (budget == -2) {
        log_peer(LOG_WARNING, "Dropped handshake from %s: unknown sources "
                 "used their share of %u handshakes per second (the rest "
                 "is kept for live clients)", peer,
                 SERVER_UNKNOWN_SHARE(SERVER_HANDSHAKES_PER_SEC));
        return -1;
    }
    if (budget == -3) {
        log_peer(LOG_WARNING, "Dropped handshake from %s: its address or "
                 "its /24 used its share of handshakes per second", peer);
        return -1;
    }

    noise_init_responder(&base, (const uint8_t *)CAMEX_NOISE_PROLOGUE,
                         sizeof(CAMEX_NOISE_PROLOGUE) - 1U,
                         ctx->server_priv, ctx->server_pub);
    if (noise_msg1_es(&base, buffer + V2_INIT_HDR, len - V2_INIT_HDR,
                      es) != 0) {
        noise_wipe(&base);
        return -1;
    }
    /* Any keystore key may authenticate message 1; the payload then has
     * to name a client that key belongs to (#5) */
    memset(&hs, 0, sizeof(hs));
    /* The global key first (entry 0 when there is one, tried free), then
     * a live client's own key (#78): its rekeys cost one trial however
     * large the keystore is; then the rest in turn */
    own = src.known != NULL ? config_ks_find(src.known->psk_key) : -1L;
    ahead = server_keystore_count > 0U &&
            server_keystore[0].owner[0] == '\0' ? 1U : 0U;
    for (n = 0U; n < server_keystore_count; ++n) {
        if (own > 0L && n == ahead) {
            i = (size_t)own;
        } else if (own > 0L && n > ahead && n <= (size_t)own) {
            i = n - 1U;
        } else {
            i = n;
        }
        if (server_keystore[i].locked) {
            continue;
        }
        /* The global key (entry 0, owner "") is always tried: clients
         * that use it are not starved by per-client key trials */
        if (i != 0U || server_keystore[i].owner[0] != '\0') {
            budget = server_key_trial_budget(&src, (long)i == own);
        } else {
            budget = 0;
        }
        if (budget == -1) {
            log_peer(LOG_WARNING, "Dropped handshake from %s: more than "
                     "%u key trials per second (per-client keys are tried "
                     "in turn)", peer, (unsigned)server_key_trials_limit());
            break;
        }
        if (budget == -2) {
            log_peer(LOG_WARNING, "Dropped handshake from %s: unknown "
                     "sources used their share of %u key trials per second "
                     "(the rest is kept for live clients)", peer,
                     (unsigned)SERVER_UNKNOWN_SHARE(
                         server_key_trials_limit()));
            break;
        }
        if (budget == -3) {
            log_peer(LOG_WARNING, "Dropped handshake from %s: its address "
                     "or its /24 used its share of key trials per second",
                     peer);
            break;
        }
        hs = base;
        if (noise_read_msg1(&hs, server_keystore[i].psk_key, es,
                            buffer + V2_INIT_HDR, len - V2_INIT_HDR,
                            payload, sizeof(payload) - 1U,
                            &payload_len) == 0) {
            used_key = server_keystore[i].psk_key;
            break;
        }
        noise_wipe(&hs);
    }
    noise_wipe(&base);
    crypto_wipe(es, sizeof(es));
    if (used_key == NULL) {
        log_peer(LOG_WARNING, "Dropped handshake from %s: no key "
                 "authenticates it", peer);
        return -1;
    }
    if (payload_len < V2_TIMESTAMP_LEN) {
        noise_wipe(&hs);
        return -1;
    }

    memcpy(message, payload + V2_TIMESTAMP_LEN,
           payload_len - V2_TIMESTAMP_LEN);
    message[payload_len - V2_TIMESTAMP_LEN] = '\0';
    client_id[0] = '\0';
    local_ip[0] = '\0';
    if (proto_parse_register(message, client_id, sizeof(client_id),
                             local_ip, sizeof(local_ip),
                             &auto_request) != 0 ||
        server_register_allowed(used_key, client_id, sizeof(client_id),
                                local_ip, auto_request, from) != 0) {
        noise_wipe(&hs);
        return -1;
    }

    /* A captured message 1 replayed later carries an old timestamp. While
     * the client has a live session (authenticated data recently; a
     * replayed message 1 alone never makes it live), its timestamps must
     * grow. Otherwise any timestamp is taken: a client whose clock went
     * back (reboot without NTP) can still connect, and a replayed message
     * 1 only leaves a session nobody can use. It never replaces a working
     * one either: a new session waits in next until the client's first
     * packet on it. */
    {
        server_client_t *owner = auto_request
                                     ? server_find_by_client_id(client_id)
                                     : NULL;
        struct in_addr a;

        if (!auto_request && inet_pton(AF_INET, local_ip, &a) == 1) {
            owner = server_find_by_ip(a.s_addr);
        }
        if (owner != NULL && owner->cur.valid && owner->last_data != 0 &&
            memcmp(owner->psk_key, used_key, 32) == 0 &&
            difftime(g_now, owner->last_data) <=
                server_client_timeout() &&
            v2_timestamp_cmp(payload, owner->hs_ts) <= 0) {
            log_peer(LOG_WARNING, "Dropped handshake from %s (%s): replayed "
                     "or out-of-date timestamp", peer,
                     client_id[0] != '\0' ? client_id : local_ip);
            noise_wipe(&hs);
            return -1;
        }
    }

    /* Never one address for two live clients (#81): the profile address
     * of this client is held by another one (a reload gave it to this
     * profile; server_revalidate_profile_addresses() ended that session
     * unless it is a manual client). Checked before server_entry_for(),
     * which would take this client's new TCP connection over. */
    if (auto_request) {
        camex_profile_t profile;
        char ip[16], mask[16];
        struct in_addr a;
        const server_client_t *holder;

        if (config_db_lookup_profile(client_id, &profile) == 0 &&
            profile.local_cidr[0] != '\0' &&
            parse_local_cidr(profile.local_cidr, ip, sizeof(ip), mask,
                             sizeof(mask)) == 0 &&
            inet_pton(AF_INET, ip, &a) == 1 &&
            (holder = server_find_by_ip(a.s_addr)) != NULL &&
            strcasecmp(holder->client_id, client_id) != 0) {
            log_peer(LOG_WARNING, "Rejected handshake from %s: the address "
                     "%s of client %s is in use by client %s", peer, ip,
                     client_id, holder->client_id[0] != '\0'
                                    ? holder->client_id : "(manual)");
            noise_wipe(&hs);
            return -1;
        }
    }

    entry = server_entry_for(used_key, client_id, local_ip, auto_request,
                             from, fd, &fresh);
    if (entry == NULL) {
        noise_wipe(&hs);
        return -1;
    }
    first = !entry->registered;   /* also restored by a failure */
    memcpy(psk_before, entry->psk_key, sizeof(psk_before));
    pending_before = entry->pending_ip_be;
    ip_before = entry->ip_be;
    /* A UDP entry with a working session keeps its address until the first
     * packet on the new one (server_handle_data), so a replayed message 1
     * cannot redirect its traffic; over TCP the connection carries it */
    if (!entry->cur.valid || fd >= 0) {
        server_set_addr(entry, from);
    }
    entry->last_seen = g_now;
    entry->auto_client = auto_request;
    server_set_id(entry, client_id);

    if (auto_request) {
        /* ip_be from the profile, so forwarding finds the client */
        camex_profile_t profile;

        if (config_db_lookup_profile(client_id, &profile) == 0 &&
            profile.local_cidr[0] != '\0') {
            char ip[16], mask[16];
            struct in_addr a;

            if (parse_local_cidr(profile.local_cidr, ip, sizeof(ip),
                                 mask, sizeof(mask)) == 0 &&
                inet_pton(AF_INET, ip, &a) == 1) {
                uint32_t offered = entry->pending_ip_be;

                server_assign_ip(entry, a.s_addr);
                /* A live client's changed profile address (#77) */
                if (entry->pending_ip_be == a.s_addr && offered != a.s_addr) {
                    log_message(LOG_INFO,
                                "Offered %s to client %s (it moves there "
                                "when it confirms the new session)",
                                profile.local_cidr, client_id);
                }
            }
        }
        if (server_build_config(entry, client_id, local_ip, reply,
                                sizeof(reply)) != 0) {
            /* No configuration (e.g. the auto pool is full) */
            noise_wipe(&hs);
            goto fail;
        }
    } else {
        /* A manual client has the address it registered with: nothing an
         * earlier auto handshake left pending applies (#77) */
        entry->pending_ip_be = 0U;
        snprintf(reply, sizeof(reply), "%s OK VER=%u\n", CAMEX_CTRL_WORD,
                 (unsigned)CAMEX_PROTO_VER);
    }

    /* Message 2 */
    if (get_random_bytes(e_seed, sizeof(e_seed)) != 0) {
        noise_wipe(&hs);
        goto fail;
    }
    server_index = server_new_index(entry);
    if (server_index == 0U) {
        noise_wipe(&hs);
        goto fail;
    }
    memcpy(resp, CAMEX_MAGIC, 4);
    resp[4] = V2_TYPE_RESP;
    v2_write_u32(resp + 5, server_index);
    v2_write_u32(resp + 9, client_index);
    resp_len = noise_write_msg2(&hs, e_seed, (const uint8_t *)reply,
                                strlen(reply), resp + V2_RESP_HDR,
                                sizeof(resp) - V2_RESP_HDR);
    crypto_wipe(e_seed, sizeof(e_seed));
    if (resp_len == 0U) {
        noise_wipe(&hs);
        goto fail;
    }
    noise_split(&hs, c1, c2);
    noise_wipe(&hs);

    /* With a current session (rekey, reconnect) the new one waits in next
     * until the client's first packet on it: until then the server keeps
     * sending on cur, so nothing in flight is lost and a lost RESP does
     * not break the working session. A first session is used at once. */
    if (entry->cur.valid) {
        /* Kept until the reply is sent: a failed send puts it back (#77) */
        next_before = entry->next;
        next_saved = 1;
        memset(&entry->next, 0, sizeof(entry->next));
        session_init(&entry->next, server_index, client_index, c2, c1, g_now);
    } else {
        session_init(&entry->cur, server_index, client_index, c2, c1, g_now);
        cur_set = 1;
    }
    crypto_wipe(c1, sizeof(c1));
    crypto_wipe(c2, sizeof(c2));
    memcpy(entry->psk_key, used_key, 32);
    memcpy(entry->hs_ts, payload, V2_TIMESTAMP_LEN);
    entry->registered = 1U;

    /* The reply goes where message 1 came from */
    if (entry->tcp_fd >= 0) {
        if (net_tcp_send_frame(entry->tcp_fd, resp,
                               V2_RESP_HDR + resp_len) < 0) {
            goto fail;
        }
    } else if (net_send_payload(net_fd, from, resp,
                                V2_RESP_HDR + resp_len) < 0) {
        goto fail;
    }
    /* The client has its address: no lease is kept for its ID, nor for
     * its addresses under another ID (#81) */
    if (auto_request) {
        server_lease_drop(server_lease_by_id(client_id));
        server_lease_drop(server_lease_by_ip(entry->ip_be));
        /* A pending address keeps its old lease until the client
         * confirms it (server_handle_data()): an offer that expires
         * gives nothing away */
    }
    if (entry->registered_at == 0) {
        entry->registered_at = g_now;
    }
    entry->last_hs = g_now;
    entry->handshakes++;
    if (first) {
        if (auto_request) {
            log_message(LOG_INFO, "Registered auto client %s", client_id);
        } else {
            log_message(LOG_INFO, "Registered client %s", local_ip);
        }
    }
    if (next_saved) {
        session_wipe(&next_before);   /* replaced by the new session */
    }
    crypto_wipe(psk_before, sizeof(psk_before));
    return 0;

fail:
    /* A slot taken for this handshake must not stay behind (#15); entries
     * that existed before keep their working sessions */
    if (fresh) {
        crypto_wipe(entry, sizeof(*entry));
        entry->tcp_fd = -1;
    } else {
        /* Addresses this handshake gave or made pending are not kept, nor
         * the session it set up: the entry keeps what it had (#77) */
        server_restore_ip(entry, ip_before, pending_before);
        if (next_saved) {
            session_wipe(&entry->next);
            entry->next = next_before;
        }
        if (cur_set) {
            /* The client never got the keys of that session */
            session_wipe(&entry->cur);
        }
        /* A pre-auth TCP slot stays pre-auth (its timeout, its log); the
         * key stays the one its sessions were made with (reloads check
         * it). The newer timestamp is kept: replays must stay older. */
        entry->registered = first ? 0U : 1U;
        memcpy(entry->psk_key, psk_before, sizeof(psk_before));
    }
    crypto_wipe(psk_before, sizeof(psk_before));
    if (next_saved) {
        crypto_wipe(&next_before, sizeof(next_before));
    }
    return -1;
}

/* DATA: find the session by the receiver index, open it, forward */
static int server_handle_data(const uint8_t *buffer, size_t len,
                              const struct sockaddr_in *from, int fd)
{
    uint8_t plain[TUN_PACKET_MAX];
    size_t plain_len = 0U;
    uint32_t idx = v2_data_receiver(buffer);
    uint32_t src_ip_be = 0;
    uint32_t dst_ip_be = 0;
    uint32_t own_ip_be;
    server_client_t *entry = NULL;
    session_t *s = NULL;
    char peer[64];
    uint32_t slot1 = idx & 0xFFFFU;

    /* The index names the slot (server_new_index()) */
    if (slot1 != 0U && slot1 <= server_max_clients &&
        server_clients[slot1 - 1U].active) {
        server_client_t *c = &server_clients[slot1 - 1U];

        if (c->cur.valid && c->cur.local_index == idx) {
            entry = c;
            s = &c->cur;
        } else if (c->next.valid && c->next.local_index == idx) {
            entry = c;
            s = &c->next;
        } else if (c->prev.valid && c->prev.local_index == idx &&
                   g_now <= c->prev_until) {
            entry = c;
            s = &c->prev;
        }
    }
    net_sockaddr_to_string(from, peer, sizeof(peer));
    /* A TCP session belongs to its connection */
    if (s == NULL || (fd >= 0 && entry->tcp_fd != fd) ||
        session_open(s, buffer, len, plain, sizeof(plain), &plain_len) != 0) {
        log_peer(LOG_WARNING, "Dropped packet from %s: unknown session or "
                 "failed to authenticate (len=%u)", peer, (unsigned)len);
        return -1;
    }

    /* The client's first packet on the new session: it becomes current,
     * the old one keeps receiving for a while */
    if (s == &entry->next) {
        session_wipe(&entry->prev);
        entry->prev = entry->cur;
        entry->prev_until = g_now + V2_PREV_SESSION_GRACE;
        entry->prev_ip_be = entry->ip_be;
        entry->cur = entry->next;
        session_wipe(&entry->next);
        s = &entry->cur;
        /* The client has its new CONFIG: its new address counts now */
        if (entry->pending_ip_be != 0U) {
            char ip[16];

            server_set_ip(entry, entry->pending_ip_be);
            entry->pending_ip_be = 0U;
            server_lease_drop(server_lease_by_ip(entry->ip_be));   /* #81 */
            ip_text(ntohl(entry->ip_be), ip, sizeof(ip));
            log_message(LOG_INFO, "Client %s moved to %s",
                        entry->client_id, ip);
        }
    }

    /* Authenticated: the client may have moved (NAT rebinding, roaming) */
    if (fd < 0 && !net_sockaddr_equal(&entry->addr, from)) {
        server_set_addr(entry, from);
    }
    entry->last_seen = g_now;
    entry->last_data = g_now;
    entry->rx_packets++;
    entry->rx_bytes += plain_len;

    if (plain_len == 0U) {
        /* Keepalive: answer, so the client can tell a live server (#10) */
        uint8_t ka[V2_OVERHEAD];
        size_t ka_len = session_seal(&entry->cur, NULL, 0U, ka, sizeof(ka));

        if (ka_len > 0U) {
            (void)server_send(entry, ka, ka_len);
        }
        return 0;
    }

    if (ipv4_parse_endpoints(plain, plain_len, &src_ip_be, &dst_ip_be) != 0) {
        return 0;
    }
    /* A client may only send from its own tunnel address (#6): the one of
     * the session the packet came on (#77) */
    own_ip_be = (s == &entry->prev && entry->prev_ip_be != 0U)
                    ? entry->prev_ip_be : entry->ip_be;
    if (own_ip_be == 0U || src_ip_be != own_ip_be) {
        char claimed[INET_ADDRSTRLEN];
        struct in_addr a;

        a.s_addr = src_ip_be;
        inet_ntop(AF_INET, &a, claimed, sizeof(claimed));
        log_peer(LOG_WARNING,
                 "Dropped packet from %s (client %s): source address "
                 "%s is not the client's tunnel address",
                 peer, entry->client_id, claimed);
        return -1;
    }
    return server_forward_packet(plain, plain_len, src_ip_be, dst_ip_be);
}

int server_forward_packet(const uint8_t *packet, size_t len,
                          uint32_t src_ip_be, uint32_t dst_ip_be)
{
    int rc;
    server_client_t *dst;
    uint8_t out[TUN_PACKET_MAX + V2_OVERHEAD];
    size_t out_len;

    (void)src_ip_be;

    dst = server_find_by_ip(dst_ip_be);
    if (dst == NULL) {
        /* Not a client: the packet is for the server's own network */
        return packet_write(packet, len);
    }
    if (!dst->cur.valid) {
        return -1;
    }
    out_len = session_seal(&dst->cur, packet, len, out, sizeof(out));
    if (out_len == 0U) {
        return -1;
    }
    if (dst->tcp_fd >= 0) {
        int accepted = 0;

        /* 1 is returned for a frame that was dropped (an older one still
         * flushing) and for one that was queued: only the queued one
         * counts as sent */
        rc = net_tcp_send_frame_ex(dst->tcp_fd, out, out_len, &accepted);
        if (accepted) {
            dst->tx_packets++;
            dst->tx_bytes += len;
        }
        return rc;
    }
    rc = server_send(dst, out, out_len);
    if (rc == 0) {
        dst->tx_packets++;
        dst->tx_bytes += len;
    }
    return rc;
}

int server_handle_packet(const uint8_t *buffer, size_t len,
                         const struct sockaddr_in *from, int fd)
{
    char peer[64];

    if (buffer == NULL || from == NULL || len == 0U) {
        return -1;
    }
    switch (v2_packet_type(buffer, len)) {
    case V2_TYPE_INIT:
        return server_handle_init(buffer, len, from, fd);
    case V2_TYPE_DATA:
        return server_handle_data(buffer, len, from, fd);
    default:
        net_sockaddr_to_string(from, peer, sizeof(peer));
        log_peer(LOG_WARNING, "Dropped packet from %s: not a v2 client "
                 "packet (len=%u)", peer, (unsigned)len);
        return -1;
    }
}

int server_socket_create(const char *bind_ip, int port,
                         const char *bind_dev)
{
    (void)bind_dev;  /* bind_dev applied via current_config.bind_dev in net_configure_socket */

    if (current_config.transport == CAMEX_TRANSPORT_TCP) {
        listen_fd = net_tcp_listen(bind_ip, port);
        if (listen_fd < 0) {
            return -1;
        }
        if (set_fd_nonblocking(listen_fd) != 0) {
            close(listen_fd);
            listen_fd = -1;
            return -1;
        }
        return 0;
    }

    /* UDP path */
    struct sockaddr_in addr;
    int reuse = 1;
    int fd;

    fd = net_open_udp_socket();
    if (fd < 0) {
        return -1;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                   (const char *)&reuse, sizeof(reuse)) < 0) {
        print_errno_message(LOG_WARNING, "setsockopt(SO_REUSEADDR)");
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind_ip != NULL && *bind_ip != '\0' &&
        inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) {
        log_message(LOG_ERR, "Invalid bind IP: %s", bind_ip);
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        print_errno_message(LOG_ERR, "bind");
        close(fd);
        return -1;
    }

    net_fd = fd;
    return 0;
}

/*
 * camex --status (#82): the server and its live clients, as a table or as
 * JSON. Built at once from the slot table; the control socket sends it.
 */
typedef struct {
    char *p;
    size_t len;
    size_t cap;
    int err;
} status_buf_t;

static void sb_printf(status_buf_t *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void sb_printf(status_buf_t *b, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (b->err) {
        return;
    }
    for (;;) {
        size_t room = b->cap - b->len;

        va_start(ap, fmt);
        n = vsnprintf(b->p != NULL ? b->p + b->len : NULL, room, fmt, ap);
        va_end(ap);
        if (n < 0) {
            b->err = 1;
            return;
        }
        if ((size_t)n < room) {
            b->len += (size_t)n;
            return;
        }
        {
            size_t cap = (b->cap == 0U ? 4096U : b->cap * 2U);
            char *grown;

            while (cap - b->len <= (size_t)n) {
                cap *= 2U;
            }
            grown = realloc(b->p, cap);
            if (grown == NULL) {
                b->err = 1;
                return;
            }
            b->p = grown;
            b->cap = cap;
        }
    }
}

/* A JSON string: client IDs are plain text, but escape anyway */
static void sb_json_str(status_buf_t *b, const char *s)
{
    sb_printf(b, "\"");
    for (; *s != '\0'; ++s) {
        unsigned char ch = (unsigned char)*s;

        if (ch == '"' || ch == '\\') {
            sb_printf(b, "\\%c", ch);
        } else if (ch < 0x20U || ch >= 0x7fU) {
            sb_printf(b, "\\u%04x", ch);
        } else {
            sb_printf(b, "%c", ch);
        }
    }
    sb_printf(b, "\"");
}

static void status_dur(char *buf, size_t size, double s)
{
    long v = s < 0.0 ? 0L : (long)s;

    if (v < 60L) {
        snprintf(buf, size, "%lds", v);
    } else if (v < 3600L) {
        snprintf(buf, size, "%ldm%02lds", v / 60L, v % 60L);
    } else if (v < 86400L) {
        snprintf(buf, size, "%ldh%02ldm", v / 3600L, (v % 3600L) / 60L);
    } else {
        snprintf(buf, size, "%ldd%02ldh", v / 86400L, (v % 86400L) / 3600L);
    }
}

static void status_bytes(char *buf, size_t size, uint64_t v)
{
    static const char *const unit[] = { "KiB", "MiB", "GiB", "TiB" };
    double d = (double)v;
    int u = -1;

    if (v < 1024U) {
        snprintf(buf, size, "%u B", (unsigned)v);
        return;
    }
    while (d >= 1024.0 && u < 3) {
        d /= 1024.0;
        u++;
    }
    snprintf(buf, size, "%.1f %s", d, unit[u]);
}

static const char *status_key(const server_client_t *c)
{
    return c->client_id[0] != '\0' &&
                   keystore_key_owned_by(c->psk_key, c->client_id)
               ? "own" : "global";
}

char *server_status(int json, size_t *len)
{
    status_buf_t b = { NULL, 0U, 0U, 0 };
    char pool[24], ip[16], peer[64];
    size_t i, used = 0U, leased = 0U;
    double up = server_started != 0 ? difftime(g_now, server_started) : 0.0;
    const char *transport = current_config.transport == CAMEX_TRANSPORT_TCP
                                ? "tcp" : "udp";
    int id_w = 2, addr_w = 7, peer_w = 4;
    long pid;

#ifdef _WIN32
    pid = (long)GetCurrentProcessId();
#else
    pid = (long)getpid();
#endif
    ip_text(server_db.globals.pool_net, ip, sizeof(ip));
    snprintf(pool, sizeof(pool), "%s/%u", ip,
             (unsigned)server_db.globals.pool_prefix);
    for (i = 0U; i < server_max_clients; ++i) {
        const server_client_t *c = &server_clients[i];
        int w;

        if (!c->active || !c->registered) {
            continue;
        }
        used++;
        w = (int)strlen(c->client_id[0] != '\0' ? c->client_id : "-");
        id_w = w > id_w ? w : id_w;
        net_sockaddr_to_string(&c->addr, peer, sizeof(peer));
        w = (int)strlen(peer);
        peer_w = w > peer_w ? w : peer_w;
        w = c->pending_ip_be != 0U ? 32 : 15;
        addr_w = w > addr_w ? w : addr_w;
    }
    leased = server_lease_used;   /* no scan of the table (#82) */

    if (json) {
        sb_printf(&b, "{\"version\":\"%s\",\"pid\":%ld,\"transport\":\"%s\","
                  "\"port\":%d,\"tunnel\":", CAMEX_VERSION, pid, transport,
                  current_config.port);
        sb_json_str(&b, current_config.local_cidr);
        sb_printf(&b, ",\"pool\":\"%s\",\"started\":%ld,\"uptime\":%.0f,"
                  "\"slots\":{\"used\":%u,\"max\":%u},\"clients\":[", pool,
                  (long)server_started, up, (unsigned)used,
                  (unsigned)server_max_clients);
    } else {
        char d[32];

        status_dur(d, sizeof(d), up);
        sb_printf(&b, "camex %s server, %s port %d, tunnel %s, pool %s, up "
                  "%s, clients %u of %u\n", CAMEX_VERSION, transport,
                  current_config.port,
                  current_config.local_cidr[0] != '\0'
                      ? current_config.local_cidr : "-",
                  pool, d, (unsigned)used, (unsigned)server_max_clients);
        if (used > 0U) {
            sb_printf(&b, "\n%-*s  %-6s  %-6s  %-*s  %-*s  %-9s  %-9s  "
                      "%-9s  %5s  %10s  %8s  %10s  %8s\n", id_w, "ID",
                      "MODE", "KEY", addr_w, "ADDRESS", peer_w, "PEER",
                      "CONNECTED", "LAST", "HANDSHAKE", "HS", "RX", "RX PKTS",
                      "TX", "TX PKTS");
        }
    }

    used = 0U;
    for (i = 0U; i < server_max_clients; ++i) {
        const server_client_t *c = &server_clients[i];
        char pend[16] = "", prev[16] = "";
        double since = c->registered_at != 0
                           ? difftime(g_now, c->registered_at) : 0.0;
        /* Authenticated data (keepalives too); a handshake is not one */
        double last = c->last_data != 0 ? difftime(g_now, c->last_data)
                                        : -1.0;
        double hs = c->last_hs != 0 ? difftime(g_now, c->last_hs) : 0.0;

        if (!c->active || !c->registered) {
            continue;
        }
        ip_text(ntohl(c->ip_be), ip, sizeof(ip));
        if (c->pending_ip_be != 0U) {
            ip_text(ntohl(c->pending_ip_be), pend, sizeof(pend));
        }
        if (c->prev.valid && c->prev_ip_be != 0U) {
            ip_text(ntohl(c->prev_ip_be), prev, sizeof(prev));
        }
        net_sockaddr_to_string(&c->addr, peer, sizeof(peer));
        if (json) {
            sb_printf(&b, "%s{\"id\":", used > 0U ? "," : "");
            if (c->client_id[0] != '\0') {
                sb_json_str(&b, c->client_id);
            } else {
                sb_printf(&b, "null");
            }
            sb_printf(&b, ",\"mode\":\"%s\",\"key\":\"%s\",\"address\":"
                      "\"%s\",\"pending\":", c->auto_client ? "auto"
                                                            : "manual",
                      status_key(c), ip);
            if (pend[0] != '\0') {
                sb_printf(&b, "\"%s\"", pend);
            } else {
                sb_printf(&b, "null");
            }
            sb_printf(&b, ",\"previous\":");
            if (prev[0] != '\0') {
                sb_printf(&b, "\"%s\"", prev);
            } else {
                sb_printf(&b, "null");
            }
            sb_printf(&b, ",\"peer\":\"%s\",\"transport\":\"%s\","
                      "\"registered\":%ld,\"connected_s\":%.0f,"
                      "\"last_packet_s\":", peer,
                      c->tcp_fd >= 0 ? "tcp" : "udp",
                      (long)c->registered_at, since);
            if (last < 0.0) {
                sb_printf(&b, "null");
            } else {
                sb_printf(&b, "%.0f", last);
            }
            sb_printf(&b, ",\"last_handshake_s\":%.0f,\"handshakes\":%u,"
                      "\"rx_packets\":%llu,\"rx_bytes\":%llu,"
                      "\"tx_packets\":%llu,\"tx_bytes\":%llu}", hs,
                      (unsigned)c->handshakes,
                      (unsigned long long)c->rx_packets,
                      (unsigned long long)c->rx_bytes,
                      (unsigned long long)c->tx_packets,
                      (unsigned long long)c->tx_bytes);
        } else {
            char addr[40], d1[32], d2[32], d3[32], rx[24], tx[24];

            if (pend[0] != '\0') {
                snprintf(addr, sizeof(addr), "%s -> %s", ip, pend);
            } else {
                snprintf(addr, sizeof(addr), "%s", ip);
            }
            status_dur(d1, sizeof(d1), since);
            if (last < 0.0) {
                snprintf(d2, sizeof(d2), "-");
            } else {
                status_dur(d2, sizeof(d2), last);
            }
            status_dur(d3, sizeof(d3), hs);
            status_bytes(rx, sizeof(rx), c->rx_bytes);
            status_bytes(tx, sizeof(tx), c->tx_bytes);
            sb_printf(&b, "%-*s  %-6s  %-6s  %-*s  %-*s  %-9s  %-9s  %-9s  "
                      "%5u  %10s  %8llu  %10s  %8llu\n", id_w,
                      c->client_id[0] != '\0' ? c->client_id : "-",
                      c->auto_client ? "auto" : "manual", status_key(c),
                      addr_w, addr, peer_w, peer, d1, d2, d3,
                      (unsigned)c->handshakes, rx,
                      (unsigned long long)c->rx_packets, tx,
                      (unsigned long long)c->tx_packets);
        }
        used++;
    }

    if (json) {
        size_t n = 0U;

        sb_printf(&b, "],\"leases\":[");
        /* Up to the last lease: the table may be much larger than the
         * leases in it */
        for (i = 0U; i < server_lease_cap && n < leased; ++i) {
            const server_lease_t *l = &server_leases[i];

            if (l->ip_be == 0U) {
                continue;
            }
            ip_text(ntohl(l->ip_be), ip, sizeof(ip));
            sb_printf(&b, "%s{\"id\":", n > 0U ? "," : "");
            sb_json_str(&b, l->id);
            sb_printf(&b, ",\"address\":\"%s\",\"gone_s\":%.0f,"
                      "\"previous\":%s}", ip, difftime(g_now, l->since),
                      l->prev ? "true" : "false");
            n++;
        }
        sb_printf(&b, "]}\n");
    } else if (leased > 0U) {
        sb_printf(&b, "\nAddresses kept for clients that are gone (#81): "
                  "%u\n", (unsigned)leased);
        for (i = 0U, used = 0U; i < server_lease_cap && used < leased; ++i) {
            const server_lease_t *l = &server_leases[i];
            char d[32];

            if (l->ip_be == 0U) {
                continue;
            }
            ip_text(ntohl(l->ip_be), ip, sizeof(ip));
            status_dur(d, sizeof(d), difftime(g_now, l->since));
            sb_printf(&b, "%-*s  %-15s  gone %s%s\n", id_w, l->id, ip, d,
                      l->prev ? " (its previous address)" : "");
            used++;
        }
    }
    if (b.err) {
        free(b.p);
        return NULL;
    }
    *len = b.len;
    return b.p;
}
