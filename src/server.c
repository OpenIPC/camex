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

#ifndef _WIN32
#include <arpa/inet.h>
#endif
#include <errno.h>
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

    return slot < server_max_clients && server_clients[slot].active &&
           (server_clients[slot].ip_be == ip ||
            server_clients[slot].pending_ip_be == ip);
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
    if (slotidx_crowded(x, server_max_clients)) {
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
}

static const char *profile_owner_of_ip(uint32_t ip_be);

server_client_t *server_find_by_ip(uint32_t ip_be)
{
    size_t i;

    if (idx_ok) {
        long k = slotidx_find(&idx_ip, slotidx_hash_u32(ip_be), match_ip,
                              &ip_be);

        return k >= 0 ? &server_clients[k] : NULL;
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

    /* Full: evict the client that was quiet longest */
    for (i = 0; i < server_max_clients; ++i) {
        if (!oldest_found || server_clients[i].last_seen < oldest_seen) {
            oldest_found = 1;
            oldest = i;
            oldest_seen = server_clients[i].last_seen;
        }
    }
    server_drop_entry(&server_clients[oldest]);
    server_clients[oldest].active = 1U;
    return &server_clients[oldest];
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
        }
        /* A handshake the client never confirmed */
        if (c->next.valid &&
            difftime(now, c->next.created) > (double)V2_PREV_SESSION_GRACE) {
            session_wipe(&c->next);
            c->pending_ip_be = 0U;   /* the old address stays (#77) */
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
            server_drop_entry(c);
        }
    }
}

/* Take a free auto-pool address for entry (#15, #75): not used by another
 * live client nor by a [client ID] profile. Addresses of expired clients
 * become free again. Returns 0 and the address (network order), or -1 if
 * the pool is full. A full pool is walked at most once a second: clients
 * expire on whole seconds, and a client that finds no address retries. */
static int server_auto_pool_take(const server_client_t *entry,
                                 uint32_t *ip_be)
{
    const uint32_t first = pool_first();
    const uint32_t size = pool_last() - first + 1U;
    const uint32_t start = (auto_pool_next >= first &&
                            auto_pool_next <= pool_last())
                               ? auto_pool_next - first : 0U;
    uint32_t n;

    if (pool_full_at == g_now && pool_full_at != 0 &&
        pool_full_net == server_db.globals.pool_net &&
        pool_full_prefix == server_db.globals.pool_prefix) {
        return -1;
    }
    for (n = 0U; n < size; ++n) {
        uint32_t host = first + (start + n) % size;
        uint32_t a = htonl(host);
        server_client_t *user = server_find_by_ip(a);

        if ((user != NULL && user != entry) || a == server_tunnel_ip ||
            profile_owner_of_ip(a) != NULL) {
            continue;
        }
        *ip_be = a;
        auto_pool_next = host < pool_last() ? host + 1U : first;
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

            /* Reuse existing auto-IP if client already has one, else the
             * address the client asks for (its previous one) when free */
            if (entry->ip_be == 0U && hint_ip != NULL &&
                hint_ip[0] != '\0') {
                struct in_addr h;
                server_client_t *user;

                if (inet_pton(AF_INET, hint_ip, &h) == 1 &&
                    pool_has(h.s_addr) && h.s_addr != server_tunnel_ip &&
                    profile_owner_of_ip(h.s_addr) == NULL &&
                    ((user = server_find_by_ip(h.s_addr)) == NULL ||
                     user == entry)) {
                    server_set_ip(entry, h.s_addr);
                }
            }
            /* The address the client was last given: a pending one when
             * it has not confirmed it yet (#77) */
            uint32_t given = entry->pending_ip_be != 0U ? entry->pending_ip_be
                                                        : entry->ip_be;

            if (pool_has(given) && given != server_tunnel_ip &&
                profile_owner_of_ip(given) == NULL) {
                /* Keep the same IP (a changed pool takes it away) */
                ip_be = given;
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
            /* Store assigned IP in client entry for packet forwarding */
            server_assign_ip(entry, ip_be);
            /* Rekeys repeat the CONFIG: log new assignments only */
            if (ip_be != old_ip_be) {
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
        server_drop_entry(c);
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

/* At most SERVER_HANDSHAKES_PER_SEC INIT messages per second */
static int server_handshake_budget(void)
{
    static time_t window = 0;
    static unsigned int count = 0U;

    if (window != g_now) {
        window = g_now;
        count = 0U;
    }
    if (count >= SERVER_HANDSHAKES_PER_SEC) {
        return -1;
    }
    count++;
    return 0;
}

/* Key trials allowed per second: SERVER_KEY_TRIALS_PER_SEC, or twice the
 * keystore when it is larger (one handshake may have to try every key) */
static size_t server_key_trials_limit(void)
{
    size_t limit = SERVER_KEY_TRIALS_PER_SEC;

    if (server_keystore_count * 2U > limit) {
        limit = server_keystore_count * 2U;
    }
    return limit;
}

/* One more key trial within server_key_trials_limit(), or -1 */
static int server_key_trial_budget(void)
{
    static time_t window = 0;
    static size_t count = 0U;

    if (window != g_now) {
        window = g_now;
        count = 0U;
    }
    if (count >= server_key_trials_limit()) {
        return -1;
    }
    count++;
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
static server_client_t *server_entry_for(const char *client_id,
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
        /* The address may be in use by a live session whose address came
         * from [defaults] or the auto pool, which profile_owner_of_ip()
         * cannot see. Do not hand it to a different keyed client. */
        if (by_ip != NULL && by_ip != by_addr &&
            strcasecmp(by_ip->client_id, client_id) != 0 &&
            keystore_has_owner(by_ip->client_id)) {
            log_peer(LOG_WARNING,
                     "Rejected handshake from %s: address %s is in use "
                     "by client %s", peer, local_ip, by_ip->client_id);
            return NULL;
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
    int fresh = 0;
    uint32_t pending_before = 0U;
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
    if (server_handshake_budget() != 0) {
        log_peer(LOG_WARNING, "Dropped handshake from %s: more than %u "
                 "handshakes per second", peer, SERVER_HANDSHAKES_PER_SEC);
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
    for (i = 0U; i < server_keystore_count; ++i) {
        if (server_keystore[i].locked) {
            continue;
        }
        /* The global key (entry 0, owner "") is always tried: clients
         * that use it are not starved by per-client key trials */
        if ((i != 0U || server_keystore[i].owner[0] != '\0') &&
            server_key_trial_budget() != 0) {
            log_peer(LOG_WARNING, "Dropped handshake from %s: more than "
                     "%u key trials per second (per-client keys are tried "
                     "in turn)", peer, (unsigned)server_key_trials_limit());
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

    entry = server_entry_for(client_id, local_ip, auto_request, from, fd,
                             &fresh);
    if (entry == NULL) {
        noise_wipe(&hs);
        return -1;
    }
    first = !entry->registered;
    pending_before = entry->pending_ip_be;
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
                server_assign_ip(entry, a.s_addr);
            }
        }
        if (server_build_config(entry, client_id, local_ip, reply,
                                sizeof(reply)) != 0) {
            /* No configuration (e.g. the auto pool is full) */
            noise_wipe(&hs);
            goto fail;
        }
    } else {
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
        session_wipe(&entry->next);
        session_init(&entry->next, server_index, client_index, c2, c1, g_now);
    } else {
        session_init(&entry->cur, server_index, client_index, c2, c1, g_now);
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
    if (first) {
        if (auto_request) {
            log_message(LOG_INFO, "Registered auto client %s", client_id);
        } else {
            log_message(LOG_INFO, "Registered client %s", local_ip);
        }
    }
    return 0;

fail:
    /* A slot taken for this handshake must not stay behind (#15); entries
     * that existed before keep their working sessions */
    if (fresh) {
        crypto_wipe(entry, sizeof(*entry));
        entry->tcp_fd = -1;
    } else {
        /* An address this handshake made pending is not given (#77) */
        entry->pending_ip_be = pending_before;
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
            server_set_ip(entry, entry->pending_ip_be);
            entry->pending_ip_be = 0U;
        }
    }

    /* Authenticated: the client may have moved (NAT rebinding, roaming) */
    if (fd < 0 && !net_sockaddr_equal(&entry->addr, from)) {
        server_set_addr(entry, from);
    }
    entry->last_seen = g_now;
    entry->last_data = g_now;

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
    return server_send(dst, out, out_len);
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
