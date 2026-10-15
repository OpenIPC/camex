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

server_client_t server_clients[CAMEX_MAX_CLIENTS];

/* Dynamic IP pool for auto-config clients when no config file is loaded.
 * Server uses 10.200.0.1, clients get 10.200.0.2 .. 10.200.0.254 */
#define AUTO_POOL_BASE "10.200.0"
#define AUTO_POOL_GATEWAY "10.200.0.1"
#define AUTO_POOL_FIRST 2U
#define AUTO_POOL_LAST  254U   /* .255 is the /24 broadcast address */
static unsigned int auto_pool_next = AUTO_POOL_FIRST;

static const char *profile_owner_of_ip(uint32_t ip_be);

/* Look up a client by its auto-assigned tunnel IP and return the IP octet */
static int server_find_auto_ip_octet(uint32_t ip_be, uint8_t *octet)
{
    struct in_addr a;
    a.s_addr = ip_be;
    {
        const char *s = inet_ntoa(a);
        int o1, o2, o3, o4;
        if (s && sscanf(s, "%d.%d.%d.%d", &o1, &o2, &o3, &o4) == 4) {
            char base[32];
            snprintf(base, sizeof(base), "%d.%d.%d", o1, o2, o3);
            if (strcmp(base, "10.200.0") == 0) {
                *octet = (uint8_t)o4;
                return 0;
            }
        }
    }
    return -1;
}

server_client_t *server_find_by_ip(uint32_t ip_be)
{
    size_t i;

    for (i = 0; i < CAMEX_MAX_CLIENTS; ++i) {
        if (server_clients[i].active && server_clients[i].ip_be == ip_be) {
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

    for (i = 0; i < CAMEX_MAX_CLIENTS; ++i) {
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

    for (i = 0; i < CAMEX_MAX_CLIENTS; ++i) {
        if (server_clients[i].active &&
            net_sockaddr_equal(&server_clients[i].addr, addr)) {
            return &server_clients[i];
        }
    }

    return NULL;
}

server_client_t *server_alloc_client(void)
{
    size_t i;
    size_t oldest = 0;
    time_t oldest_seen = 0;
    int oldest_found = 0;

    for (i = 0; i < CAMEX_MAX_CLIENTS; ++i) {
        if (!server_clients[i].active) {
            memset(&server_clients[i], 0, sizeof(server_clients[i]));
            server_clients[i].tcp_fd = -1;
            server_clients[i].active = 1U;
            return &server_clients[i];
        }

        if (!oldest_found || server_clients[i].last_seen < oldest_seen) {
            oldest_found = 1;
            oldest = i;
            oldest_seen = server_clients[i].last_seen;
        }
    }

    /* Full: evict the client that was quiet longest */
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

void server_expire_clients(void)
{
    static time_t last_expire_check = 0;
    size_t i;
    time_t now = (g_now != 0) ? g_now : time(NULL);

    if (now == last_expire_check) {
        return;
    }
    last_expire_check = now;

    for (i = 0; i < CAMEX_MAX_CLIENTS; ++i) {
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
        }
        /* Pre-auth: counted from the accept, so frames that arrive without
         * a handshake cannot keep an unregistered connection open. */
        if (preauth ? difftime(now, c->connected_at) >
                          (double)CAMEX_TCP_PREAUTH_TIMEOUT
                    : difftime(now, c->last_seen) >
                          (double)CAMEX_CLIENT_TIMEOUT) {
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

/* Take a free auto-pool address for entry (#15): .2-.254, not used by
 * another live client nor by a [client ID] profile. Addresses of expired
 * clients become free again. Returns 0 and the last octet, or -1 if the
 * pool is full. */
static int server_auto_pool_take(const server_client_t *entry, uint8_t *octet)
{
    const unsigned int size = AUTO_POOL_LAST - AUTO_POOL_FIRST + 1U;
    unsigned int n;

    for (n = 0U; n < size; ++n) {
        unsigned int o = AUTO_POOL_FIRST +
                         (auto_pool_next - AUTO_POOL_FIRST + n) % size;
        char ip[16];
        struct in_addr a;
        server_client_t *user;

        snprintf(ip, sizeof(ip), AUTO_POOL_BASE ".%u", o);
        if (inet_pton(AF_INET, ip, &a) != 1) {
            continue;
        }
        user = server_find_by_ip(a.s_addr);
        if ((user != NULL && user != entry) ||
            profile_owner_of_ip(a.s_addr) != NULL) {
            continue;
        }
        *octet = (uint8_t)o;
        auto_pool_next = (o < AUTO_POOL_LAST) ? o + 1U : AUTO_POOL_FIRST;
        return 0;
    }
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
    for (i = 0U; i < CAMEX_MAX_CLIENTS; ++i) {
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
    uint32_t old_ip_be = entry != NULL ? entry->ip_be : 0U;

    if (config_db_lookup_profile(client_id, &profile) != 0) {
        log_message(LOG_ERR, "Missing config for client %s",
                    client_id != NULL ? client_id : "(null)");
        return -1;
    }

    if (profile.local_cidr[0] == '\0' || profile.gateway_ip[0] == '\0') {
        /* No config file loaded — auto-assign dynamic IP for auto-config client */
        if (entry != NULL && client_id != NULL && client_id[0] != '\0') {
            uint8_t octet;

            /* Reuse existing auto-IP if client already has one, else the
             * address the client asks for (its previous one) when free */
            if (entry->ip_be == 0U && hint_ip != NULL &&
                hint_ip[0] != '\0') {
                struct in_addr h;
                server_client_t *user;

                if (inet_pton(AF_INET, hint_ip, &h) == 1 &&
                    server_find_auto_ip_octet(h.s_addr, &octet) == 0 &&
                    octet >= AUTO_POOL_FIRST && octet <= AUTO_POOL_LAST &&
                    profile_owner_of_ip(h.s_addr) == NULL &&
                    ((user = server_find_by_ip(h.s_addr)) == NULL ||
                     user == entry)) {
                    entry->ip_be = h.s_addr;
                }
            }
            if (entry->ip_be != 0U &&
                server_find_auto_ip_octet(entry->ip_be, &octet) == 0 &&
                octet >= AUTO_POOL_FIRST && octet <= AUTO_POOL_LAST &&
                profile_owner_of_ip(entry->ip_be) == NULL) {
                /* Keep the same IP */
            } else if (server_auto_pool_take(entry, &octet) != 0) {
                log_peer(LOG_WARNING,
                         "Auto pool " AUTO_POOL_BASE ".0/24 is full: "
                         "no address for client %s", client_id);
                return -1;
            }
            snprintf(profile.local_cidr, sizeof(profile.local_cidr),
                     AUTO_POOL_BASE ".%u/24", (unsigned)octet);
            snprintf(profile.gateway_ip, sizeof(profile.gateway_ip),
                     "%s", AUTO_POOL_GATEWAY);
            /* Store assigned IP in client entry for packet forwarding */
            {
                struct in_addr a;
                char ip[16];
                snprintf(ip, sizeof(ip), AUTO_POOL_BASE ".%u",
                         (unsigned)octet);
                if (inet_pton(AF_INET, ip, &a) == 1) {
                    entry->ip_be = a.s_addr;
                }
            }
            /* Rekeys repeat the CONFIG: log new assignments only */
            if (entry->ip_be != old_ip_be) {
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
    size_t i;

    if (key == NULL) {
        return "";
    }
    for (i = 0U; i < server_keystore_count; ++i) {
        if (!server_keystore[i].locked &&
            memcmp(server_keystore[i].psk_key, key, 32) == 0) {
            return server_keystore[i].owner;
        }
    }
    return "";
}

static int keystore_has_owner(const char *client_id)
{
    size_t i;

    for (i = 0U; i < server_keystore_count; ++i) {
        if (server_keystore[i].owner[0] != '\0' &&
            strcasecmp(server_keystore[i].owner, client_id) == 0) {
            return 1;
        }
    }
    return 0;
}

static int keystore_key_owned_by(const uint8_t *key, const char *client_id)
{
    size_t i;

    if (key == NULL) {
        return 0;
    }
    for (i = 0U; i < server_keystore_count; ++i) {
        if (server_keystore[i].owner[0] != '\0' &&
            !server_keystore[i].locked &&
            strcasecmp(server_keystore[i].owner, client_id) == 0 &&
            memcmp(server_keystore[i].psk_key, key, 32) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Tunnel IP configured for client_id in its own [client ID] section. */
static int profile_ip(const char *client_id, uint32_t *ip_be)
{
    camex_server_profile_t *p = config_db_find_client(client_id);
    char ip[16];
    char mask[16];
    struct in_addr a;

    if (p == NULL || p->profile.local_cidr[0] == '\0' ||
        parse_local_cidr(p->profile.local_cidr, ip, sizeof(ip),
                         mask, sizeof(mask)) != 0 ||
        inet_pton(AF_INET, ip, &a) != 1) {
        return -1;
    }
    *ip_be = a.s_addr;
    return 0;
}

/* The [client ID] section whose local_cidr address is ip_be, if any. */
static const char *profile_owner_of_ip(uint32_t ip_be)
{
    size_t i;

    for (i = 0U; i < CAMEX_MAX_CLIENTS; ++i) {
        uint32_t want;

        if (server_db.clients[i].active &&
            profile_ip(server_db.clients[i].client_id, &want) == 0 &&
            want == ip_be) {
            return server_db.clients[i].client_id;
        }
    }
    return NULL;
}

static int keystore_contains(const uint8_t *key)
{
    size_t i;

    for (i = 0U; i < server_keystore_count; ++i) {
        if (!server_keystore[i].locked &&
            memcmp(server_keystore[i].psk_key, key, 32) == 0) {
            return 1;
        }
    }
    return 0;
}

static int keystore_is_global(const uint8_t *key)
{
    size_t i;

    for (i = 0U; i < server_keystore_count; ++i) {
        if (server_keystore[i].owner[0] == '\0' &&
            !server_keystore[i].locked &&
            memcmp(server_keystore[i].psk_key, key, 32) == 0) {
            return 1;
        }
    }
    return 0;
}

void server_revalidate_keys(void)
{
    size_t i;

    /* After a config reload (SIGHUP) every live session must still be
     * allowed with the key it registered with: a removed or changed key,
     * or a key that now belongs to another client, ends it (#16 L3). */
    for (i = 0U; i < CAMEX_MAX_CLIENTS; ++i) {
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

/* A random session index no session uses, or 0 */
static uint32_t server_new_index(void)
{
    int tries;

    for (tries = 0; tries < 16; ++tries) {
        uint32_t idx = v2_new_index();
        size_t i;
        int used = 0;

        if (idx == 0U) {
            return 0U;
        }
        for (i = 0U; i < CAMEX_MAX_CLIENTS && !used; ++i) {
            const server_client_t *c = &server_clients[i];

            used = c->active &&
                   ((c->cur.valid && c->cur.local_index == idx) ||
                    (c->next.valid && c->next.local_index == idx) ||
                    (c->prev.valid && c->prev.local_index == idx));
        }
        if (!used) {
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
    for (j = 0; j < CAMEX_MAX_CLIENTS; ++j) {
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
        } else if (by_addr != NULL) {
            entry = by_addr;
        } else {
            entry = server_alloc_client();
            if (entry == NULL) {
                return NULL;
            }
            *fresh = 1;
        }
        entry->ip_be = addr.s_addr;
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
                (double)CAMEX_CLIENT_TIMEOUT &&
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
    /* A UDP entry with a working session keeps its address until the first
     * packet on the new one (server_handle_data), so a replayed message 1
     * cannot redirect its traffic; over TCP the connection carries it */
    if (!entry->cur.valid || fd >= 0) {
        entry->addr = *from;
    }
    entry->last_seen = g_now;
    entry->auto_client = auto_request;
    snprintf(entry->client_id, sizeof(entry->client_id), "%s", client_id);

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
                entry->ip_be = a.s_addr;
            }
        }
        if (server_build_config(entry, client_id, local_ip, reply,
                                sizeof(reply)) != 0) {
            /* No configuration (e.g. the auto pool is full): a slot taken
             * for this handshake must not stay behind (#15) */
            if (fresh) {
                crypto_wipe(entry, sizeof(*entry));
                entry->tcp_fd = -1;
            }
            noise_wipe(&hs);
            return -1;
        }
    } else {
        snprintf(reply, sizeof(reply), "%s OK VER=%u\n", CAMEX_CTRL_WORD,
                 (unsigned)CAMEX_PROTO_VER);
    }

    /* Message 2 */
    if (get_random_bytes(e_seed, sizeof(e_seed)) != 0) {
        noise_wipe(&hs);
        return -1;
    }
    server_index = server_new_index();
    if (server_index == 0U) {
        noise_wipe(&hs);
        return -1;
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
        return -1;
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
            return -1;
        }
    } else if (net_send_payload(net_fd, from, resp,
                                V2_RESP_HDR + resp_len) < 0) {
        return -1;
    }
    if (first) {
        if (auto_request) {
            log_message(LOG_INFO, "Registered auto client %s", client_id);
        } else {
            log_message(LOG_INFO, "Registered client %s", local_ip);
        }
    }
    return 0;
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
    server_client_t *entry = NULL;
    session_t *s = NULL;
    char peer[64];
    size_t i;

    for (i = 0U; i < CAMEX_MAX_CLIENTS && s == NULL; ++i) {
        server_client_t *c = &server_clients[i];

        if (!c->active) {
            continue;
        }
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
        entry->cur = entry->next;
        session_wipe(&entry->next);
        s = &entry->cur;
    }

    /* Authenticated: the client may have moved (NAT rebinding, roaming) */
    if (fd < 0 && !net_sockaddr_equal(&entry->addr, from)) {
        entry->addr = *from;
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
    /* A client may only send from its own tunnel address (#6). */
    if (entry->ip_be == 0U || src_ip_be != entry->ip_be) {
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
