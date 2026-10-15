/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * server.c — server-side client tracking, packet forwarding, keystore lookup
 *
 */

#include "server.h"
#include "client.h"
#include "crypto.h"
#include "log.h"
#include "util.h"
#include "proto.h"
#include "net.h"
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

/* Sessions that ended (#7): the client's nonce prefix and key, and the
 * highest sequence number seen with them. A REGISTER of such a session is
 * accepted only with a newer sequence number, so a captured REGISTER cannot
 * restart an old session after its entry was replaced or expired. Bounded:
 * older sessions are forgotten (per-session keys in #17 close this fully). */
#define SERVER_ENDED_SESSIONS 256U

typedef struct {
    uint8_t nonce_prefix[4];
    uint8_t key[32];
    uint8_t used;
    uint64_t seq_max;
} server_ended_session_t;

static server_ended_session_t server_ended[SERVER_ENDED_SESSIONS];
static size_t server_ended_next = 0U;

/* A session is identified by the client's nonce prefix together with the
 * key it authenticates with: another client cannot claim it by reusing
 * the prefix under its own key. */
static int server_session_is(const uint8_t prefix_a[4], const uint8_t *key_a,
                             const uint8_t prefix_b[4], const uint8_t *key_b)
{
    return memcmp(prefix_a, prefix_b, 4) == 0 &&
           memcmp(key_a, key_b, 32) == 0;
}

static server_ended_session_t *server_find_ended(const uint8_t prefix[4],
                                                 const uint8_t *key)
{
    size_t i;

    for (i = 0U; i < SERVER_ENDED_SESSIONS; ++i) {
        if (server_ended[i].used &&
            server_session_is(server_ended[i].nonce_prefix,
                              server_ended[i].key, prefix, key)) {
            return &server_ended[i];
        }
    }
    return NULL;
}

void server_forget_session(server_client_t *entry)
{
    server_ended_session_t *slot;
    size_t i;

    if (entry == NULL || !entry->has_recv_prefix) {
        return;
    }
    slot = server_find_ended(entry->recv_nonce_prefix, entry->psk_key);
    for (i = 0U; slot == NULL && i < SERVER_ENDED_SESSIONS; ++i) {
        if (!server_ended[i].used) {
            slot = &server_ended[i];
        }
    }
    if (slot == NULL) {
        /* Full: replace in round-robin order, i.e. the oldest record */
        slot = &server_ended[server_ended_next];
        server_ended_next = (server_ended_next + 1U) % SERVER_ENDED_SESSIONS;
    }
    if (!slot->used) {
        memcpy(slot->nonce_prefix, entry->recv_nonce_prefix, 4);
        memcpy(slot->key, entry->psk_key, 32);
        slot->used = 1U;
        slot->seq_max = 0U;
    }
    if (entry->recv_seq_max > slot->seq_max) {
        slot->seq_max = entry->recv_seq_max;
    }
    entry->has_recv_prefix = 0U;
}

/* Result of the replay check of an encrypted REGISTER */
typedef struct {
    server_client_t *live;    /* live session it continues, or NULL */
    int resume;               /* continue above resume_seq_max */
    uint64_t resume_seq_max;
} server_register_state_t;

/* Replay check for an encrypted REGISTER (#7), done before anything else
 * acts on it (including the rate limit, so a dropped REGISTER is still
 * marked as seen). Returns 0 to accept, -1 for a replay. */
static int server_register_replay_check(const server_register_seq_t *rseq,
                                        server_register_state_t *state)
{
    server_ended_session_t *ended;
    size_t i;

    memset(state, 0, sizeof(*state));
    if (rseq == NULL || rseq->key == NULL) {
        return 0;  /* plaintext: nothing to check */
    }
    for (i = 0U; i < CAMEX_MAX_CLIENTS; ++i) {
        server_client_t *c = &server_clients[i];

        if (c->active && c->has_recv_prefix &&
            server_session_is(c->recv_nonce_prefix, c->psk_key,
                              rseq->nonce_prefix, rseq->key)) {
            if (replay_check(&c->recv_seq_max, &c->recv_window,
                             rseq->seq) != 0) {
                return -1;
            }
            /* If it ends up in another entry (a new TCP connection), that
             * one continues above what this session has seen. */
            state->live = c;
            state->resume = 1;
            state->resume_seq_max = c->recv_seq_max;
            return 0;
        }
    }
    ended = server_find_ended(rseq->nonce_prefix, rseq->key);
    if (ended != NULL) {
        if (rseq->seq <= ended->seq_max) {
            return -1;
        }
        state->resume = 1;
        state->resume_seq_max = ended->seq_max;
    }
    return 0;
}

/* Start the replay state of a session registered with rseq in entry.
 * Uses the values captured by the check: the ended-session record may
 * have been replaced since (slot eviction writes to the same table). */
static void server_begin_session(server_client_t *entry,
                                 const server_register_seq_t *rseq,
                                 const server_register_state_t *state)
{
    server_ended_session_t *ended;

    server_forget_session(entry);
    server_reset_replay(entry);
    if (rseq == NULL || rseq->key == NULL) {
        return;
    }
    memcpy(entry->recv_nonce_prefix, rseq->nonce_prefix, 4);
    entry->has_recv_prefix = 1U;
    if (state->resume) {
        entry->recv_seq_max = state->resume_seq_max;
        entry->recv_window = ~(uint64_t)0;
    }
    ended = server_find_ended(rseq->nonce_prefix, rseq->key);
    if (ended != NULL) {
        ended->used = 0U;  /* live again: forgotten again when it ends */
    }
    (void)replay_check(&entry->recv_seq_max, &entry->recv_window, rseq->seq);
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
            if (get_random_bytes(
                    server_clients[i].send_nonce_prefix,
                    sizeof(server_clients[i].send_nonce_prefix)) != 0) {
                log_message(LOG_WARNING,
                            "getrandom failed: using weak nonce prefix");
            }
            server_clients[i].active = 1U;
            return &server_clients[i];
        }

        if (!oldest_found || server_clients[i].last_seen < oldest_seen) {
            oldest_found = 1;
            oldest = i;
            oldest_seen = server_clients[i].last_seen;
        }
    }

    /* Close old TCP fd before evicting, then memset */
    int old_tcp_fd = server_clients[oldest].tcp_fd;
    server_forget_session(&server_clients[oldest]);
    memset(&server_clients[oldest], 0, sizeof(server_clients[oldest]));
    server_clients[oldest].tcp_fd = -1;
    if (old_tcp_fd >= 0) {
        net_tcp_close(old_tcp_fd);
    }
    if (get_random_bytes(
            server_clients[oldest].send_nonce_prefix,
            sizeof(server_clients[oldest].send_nonce_prefix)) != 0) {
        log_message(LOG_WARNING,
                    "getrandom failed: using weak nonce prefix");
    }
    server_clients[oldest].active = 1U;
    return &server_clients[oldest];
}

server_client_t *server_upsert_client(uint32_t ip_be,
                                      const struct sockaddr_in *addr)
{
    server_client_t *entry = NULL;

    if (addr != NULL) {
        entry = server_find_by_addr(addr);
    }
    if (entry == NULL) {
        entry = server_find_by_ip(ip_be);
    }
    if (entry == NULL) {
        entry = server_alloc_client();
    }
    if (entry == NULL) {
        return NULL;
    }

    entry->ip_be = ip_be;
    if (addr != NULL) {
        entry->addr = *addr;
    }
    entry->last_seen = g_now;
    return entry;
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
        int preauth = server_clients[i].tcp_fd >= 0 &&
                      !server_clients[i].registered;

        /* Pre-auth: counted from the accept, so frames that are accepted
         * without a REGISTER (plaintext keepalive or data) cannot keep an
         * unregistered connection open. */
        if (server_clients[i].active &&
            (preauth ? difftime(now, server_clients[i].connected_at) >
                           (double)CAMEX_TCP_PREAUTH_TIMEOUT
                     : difftime(now, server_clients[i].last_seen) >
                           (double)CAMEX_CLIENT_TIMEOUT)) {
            char ipbuf[INET_ADDRSTRLEN];
            struct in_addr addr;

            addr.s_addr = server_clients[i].ip_be;
            inet_ntop(AF_INET, &addr, ipbuf, sizeof(ipbuf));
            if (preauth) {
                char peer[64];

                net_sockaddr_to_string(&server_clients[i].addr, peer,
                                       sizeof(peer));
                log_peer(LOG_INFO,
                         "Closed TCP connection from %s: no REGISTER "
                         "within %ds", peer, CAMEX_TCP_PREAUTH_TIMEOUT);
            } else {
                log_message(LOG_INFO, "Expired client %s", ipbuf);
            }
            if (server_clients[i].tcp_fd >= 0) {
                net_tcp_close(server_clients[i].tcp_fd);
                server_clients[i].tcp_fd = -1;
            }
            server_forget_session(&server_clients[i]);
            memset(&server_clients[i], 0, sizeof(server_clients[i]));
        }
    }
}

void server_reset_replay(server_client_t *entry)
{
    if (entry == NULL) {
        return;
    }

    entry->recv_seq_max = 0;
    entry->recv_window = 0;

    /* Rotate nonce_prefix on reconnection to prevent cross-session replay. */
    if (get_random_bytes(entry->send_nonce_prefix,
                         sizeof(entry->send_nonce_prefix)) != 0) {
        log_message(LOG_WARNING,
                    "getrandom failed: using weak nonce prefix for reconnect");
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
        if (c->tcp_fd >= 0) {
            net_tcp_close(c->tcp_fd);
        }
        server_forget_session(c);
        memset(c, 0, sizeof(*c));
        c->tcp_fd = -1;
    }
}

int server_send_config_response(const struct sockaddr_in *from,
                                server_client_t *entry,
                                const char *client_id)
{
    char message[CAMEX_CONTROL_MAX];
    uint8_t packet[CAMEX_HDR_LEN + 1 + CAMEX_CONTROL_MAX + 16U];
    size_t packet_len = 0U;
    camex_profile_t profile;

    if (from == NULL) {
        return -1;
    }

    if (config_db_lookup_profile(client_id, &profile) != 0) {
        log_message(LOG_ERR, "Missing config for client %s",
                    client_id != NULL ? client_id : "(null)");
        return -1;
    }

    if (profile.local_cidr[0] == '\0' || profile.gateway_ip[0] == '\0') {
        /* No config file loaded — auto-assign dynamic IP for auto-config client */
        if (entry != NULL && client_id != NULL && client_id[0] != '\0') {
            uint8_t octet;

            /* Reuse existing auto-IP if client already has one */
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
            log_message(LOG_INFO,
                        "Auto-assigned %s to client %s (no config file)",
                        profile.local_cidr, client_id);

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
        } else {
            log_message(LOG_ERR, "Incomplete config for client %s",
                        client_id != NULL ? client_id : "(null)");
            return -1;
        }
    }

    if (proto_build_config(client_id, &profile,
                           message, sizeof(message)) != 0) {
        return -1;
    }

    if (current_config.encrypt) {
        if (entry == NULL) {
            log_message(LOG_ERR, "No client entry for config response");
            return -1;
        }
        {
            uint64_t seq = entry->send_seq;
            if (crypto_encrypt_packet(CAMEX_PACKET_CONFIG, seq,
                    entry->send_nonce_prefix,
                    is_zero_key(entry->psk_key) ? NULL : entry->psk_key,
                    (const uint8_t *)message, strlen(message),
                    packet, sizeof(packet), &packet_len) != 0) {
                return -1;
            }
            entry->send_seq = seq + 1U;
        }
        if (entry->tcp_fd >= 0) {
            return net_tcp_send_frame(entry->tcp_fd, packet, packet_len);
        }
        return net_send_payload(net_fd, &entry->addr, packet, packet_len);
    }

    if (entry != NULL && entry->tcp_fd >= 0) {
        return net_tcp_send_frame(entry->tcp_fd,
                                  (const uint8_t *)message, strlen(message));
    }
    if (current_config.transport == CAMEX_TRANSPORT_TCP) {
        log_message(LOG_WARNING,
                    "Cannot send config to %s: no TCP fd (check server_find_by_addr)",
                    client_id != NULL ? client_id : "(null)");
        return -1;
    }
    return net_send_text(net_fd, from, message);
}

/* Answer a client KEEPALIVE so the client can tell a live server from a
 * dead one (#10). */
static void server_send_keepalive(server_client_t *entry)
{
    uint8_t packet[CAMEX_HDR_LEN + 1U + 16U];
    size_t packet_len = 0U;
    int rc;

    if (entry == NULL) {
        return;
    }

    if (current_config.encrypt) {
        uint64_t seq = entry->send_seq;

        if (crypto_encrypt_packet(CAMEX_PACKET_KEEPALIVE, seq,
                entry->send_nonce_prefix,
                is_zero_key(entry->psk_key) ? NULL : entry->psk_key,
                NULL, 0, packet, sizeof(packet), &packet_len) != 0) {
            return;
        }
        entry->send_seq = seq + 1U;
    } else {
        memcpy(packet, CAMEX_MAGIC, 4);
        packet[4] = CAMEX_PACKET_KEEPALIVE;
        packet_len = 5U;
    }

    if (entry->tcp_fd >= 0) {
        rc = net_tcp_send_frame(entry->tcp_fd, packet, packet_len);
    } else {
        rc = net_send_payload(net_fd, &entry->addr, packet, packet_len);
    }
    /* rc > 0 on TCP: an earlier frame to this client is still queued and
     * the reply is dropped.  That is fine for liveness: the queued frame
     * reaches the client as soon as the socket drains, and the client
     * counts any authenticated frame as server activity, not only
     * keepalive replies. */
    if (rc < 0) {
        log_message(LOG_DEBUG, "Failed to answer keepalive from %s",
                    entry->client_id);
    } else if (rc > 0) {
        log_message(LOG_DEBUG, "Keepalive reply to %s dropped: "
                    "send queue busy", entry->client_id);
    }
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
        if (memcmp(server_keystore[i].psk_key, key, 32) == 0) {
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
        if (memcmp(server_keystore[i].psk_key, key, 32) == 0) {
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
            memcmp(server_keystore[i].psk_key, key, 32) == 0) {
            return 1;
        }
    }
    return 0;
}

void server_revalidate_keys(void)
{
    size_t i;

    if (!current_config.encrypt) {
        return;
    }
    /* After a config reload (SIGHUP) every live session must still be
     * allowed with the key it registered with: a removed or changed key,
     * or a key that now belongs to another client, ends it (#16 L3). */
    for (i = 0U; i < CAMEX_MAX_CLIENTS; ++i) {
        server_client_t *c = &server_clients[i];
        const char *owner;
        int allowed;

        if (!c->active || is_zero_key(c->psk_key)) {
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
        if (c->tcp_fd >= 0) {
            net_tcp_close(c->tcp_fd);
        }
        server_forget_session(c);
        memset(c, 0, sizeof(*c));
        c->tcp_fd = -1;
    }
}

/*
 * May a REGISTER authenticated with key claim client_id / local_ip?
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

    if (current_config.encrypt) {
        if (client_id[0] == '\0' && !global_key) {
            snprintf(client_id, client_id_size, "%s", owner);
        }
        /* The named client's own key always counts as its key, even when
         * the same bytes are also the global key or another client's key
         * (keystore_owner() only reports the first match). */
        if (client_id[0] != '\0' &&
            !keystore_key_owned_by(key, client_id) &&
            (!global_key || keystore_has_owner(client_id))) {
            log_peer(LOG_WARNING,
                     "Rejected REGISTER from %s: key does not belong "
                     "to client %s", peer, client_id);
            return -1;
        }
    }

    /* Plaintext mode has no keys and no identities to protect: manual
     * registration there stays as it was. */
    if (current_config.encrypt && !auto_request) {
        struct in_addr claimed;
        uint32_t want;
        const char *ip_owner;

        if (inet_pton(AF_INET, local_ip, &claimed) != 1) {
            return -1;
        }
        if (client_id[0] != '\0' && profile_ip(client_id, &want) == 0 &&
            want != claimed.s_addr) {
            log_peer(LOG_WARNING,
                     "Rejected REGISTER from %s: client %s claims %s, "
                     "configured address differs", peer, client_id,
                     local_ip);
            return -1;
        }
        ip_owner = profile_owner_of_ip(claimed.s_addr);
        if (ip_owner != NULL &&
            strcasecmp(ip_owner, client_id) != 0 &&
            keystore_has_owner(ip_owner)) {
            log_peer(LOG_WARNING,
                     "Rejected REGISTER from %s: address %s belongs "
                     "to client %s", peer, local_ip, ip_owner);
            return -1;
        }
    }

    return 0;
}

static void server_log_register_replay(const struct sockaddr_in *from,
                                       const char *who)
{
    char peer[64];

    net_sockaddr_to_string(from, peer, sizeof(peer));
    log_peer(LOG_WARNING,
             "Dropped REGISTER from %s (%s): replayed or out-of-date "
             "sequence number", peer, who);
}

int server_handle_plain_register(const uint8_t *buffer, size_t len,
                                 const struct sockaddr_in *from,
                                 const uint8_t *used_key, int fd,
                                 const server_register_seq_t *rseq)
{
    server_register_state_t rstate;
    int fresh_entry = 0;
    char message[CAMEX_CONTROL_MAX];
    char local_ip[16];
    char client_id[CAMEX_CLIENT_ID_LEN];
    uint8_t auto_request = 0U;
    server_client_t *entry;
    struct in_addr addr;

    if (buffer == NULL || from == NULL || len == 0U) {
        return -1;
    }

    if (len >= sizeof(message)) {
        len = sizeof(message) - 1U;
    }

    memcpy(message, buffer, len);
    message[len] = '\0';
    local_ip[0] = '\0';
    client_id[0] = '\0';

    if (proto_parse_register(message, client_id, sizeof(client_id),
                             local_ip, sizeof(local_ip),
                             &auto_request) != 0) {
        return -1;
    }

    if (server_register_allowed(used_key, client_id, sizeof(client_id),
                                local_ip, auto_request, from) != 0) {
        return -1;
    }

    if (server_register_replay_check(rseq, &rstate) != 0) {
        server_log_register_replay(from,
                                   auto_request ? client_id : local_ip);
        return -1;
    }

    /* Rate-limit: reject repeated REGISTER from same addr within 5 seconds */
    {
        server_client_t *existing = server_find_by_addr(from);
        if (existing != NULL && existing->last_register_time != 0 &&
            difftime(g_now, existing->last_register_time) < 5.0) {
            return 0;  /* rate-limited: silently ignore */
        }
    }

    if (auto_request) {
        /* allow_unknown_clients = no: only IDs with a [client ID] section
         * get a configuration (#15). */
        if (server_db.globals.deny_unknown_clients &&
            config_db_find_client(client_id) == NULL) {
            char peer[64];

            net_sockaddr_to_string(from, peer, sizeof(peer));
            log_peer(LOG_WARNING,
                     "Rejected REGISTER from %s: unknown client ID %s "
                     "(allow_unknown_clients = no)", peer, client_id);
            return -1;
        }
        entry = server_find_by_client_id(client_id);
        if (entry != NULL) {
            if (!net_sockaddr_equal(&entry->addr, from)) {
                char old_peer[64], new_peer[64];
                net_sockaddr_to_string(&entry->addr, old_peer, sizeof(old_peer));
                net_sockaddr_to_string(from, new_peer, sizeof(new_peer));
                log_message(LOG_INFO, "Client %s reconnected: %s -> %s",
                            client_id, old_peer, new_peer);
            }
            entry->addr = *from;
            entry->last_seen = g_now;
            /* BUGFIX: on reconnect, this branch previously left entry->tcp_fd
             * pointing at the OLD (now-closed) connection's fd — every reply
             * (config response, forwarded data) kept going to a stale/wrong
             * descriptor, causing silent corruption and periodic forced
             * disconnects. The fresh TCP connection's own slot (allocated by
             * drain_tcp_server_accept, matched below by tcp_fd) never got this
             * client_id, so it leaked until CAMEX_CLIENT_TIMEOUT expired it.
             * Fix: adopt the current fd here, and reclaim the orphaned slot. */
            if (fd >= 0 && entry->tcp_fd != fd) {
                size_t j;
                for (j = 0; j < CAMEX_MAX_CLIENTS; ++j) {
                    if (server_clients[j].active &&
                        server_clients[j].tcp_fd == fd &&
                        &server_clients[j] != entry) {
                        server_forget_session(&server_clients[j]);
                        memset(&server_clients[j], 0, sizeof(server_clients[j]));
                    }
                }
                /* The old connection is gone: close it (it is no longer
                 * referenced by any slot and would leak), and drop its
                 * partial-read state, which belongs to the old byte stream. */
                if (entry->tcp_fd >= 0) {
                    net_tcp_close(entry->tcp_fd);
                }
                entry->tcp_fd = fd;
                memset(&entry->recv_state, 0, sizeof(entry->recv_state));
            }
        } else {
            /* TCP clients already have a slot allocated by drain_tcp_server_accept.
             * Reuse it instead of creating a second entry — otherwise tcp_fd
             * is lost and the config response goes via UDP. */
            server_client_t *by_addr = server_find_by_addr(from);
            if (by_addr != NULL && by_addr->tcp_fd >= 0) {
                entry = by_addr;
            } else {
                entry = server_alloc_client();
                if (entry == NULL) {
                    log_message(LOG_WARNING, "No free slots for client %s",
                                client_id);
                    return -1;
                }
                fresh_entry = 1;
            }
            entry->addr = *from;
            entry->last_seen = g_now;
        }
        entry->auto_client = 1U;
        snprintf(entry->client_id, sizeof(entry->client_id), "%s", client_id);
        if (rstate.live != entry) {
            server_begin_session(entry, rseq, &rstate);
        }
        entry->registered = 1U;
        entry->last_register_time = g_now;
        if (used_key != NULL) {
            memcpy(entry->psk_key, used_key, 32);
        }
        /* Set ip_be from profile's local_cidr so server_find_by_ip()
         * works even when NAT changes the client's UDP source port. */
        {
            camex_profile_t profile;
            if (config_db_lookup_profile(client_id, &profile) == 0 &&
                profile.local_cidr[0] != '\0') {
                char ip[16], mask[16];
                if (parse_local_cidr(profile.local_cidr, ip, sizeof(ip),
                                     mask, sizeof(mask)) == 0) {
                    struct in_addr a;
                    if (inet_pton(AF_INET, ip, &a) == 1) {
                        entry->ip_be = a.s_addr;
                    }
                }
            }
        }
        if (server_send_config_response(from, entry, client_id) != 0) {
            /* No configuration (e.g. the auto pool is full): a slot taken
             * for this REGISTER must not stay behind, or failed attempts
             * fill the table and evict live clients (#15). */
            if (fresh_entry) {
                server_forget_session(entry);
                memset(entry, 0, sizeof(*entry));
                entry->tcp_fd = -1;
            }
            return -1;
        }
        log_message(LOG_INFO, "Registered auto client %s", client_id);
        return 0;
    }

    if (inet_pton(AF_INET, local_ip, &addr) != 1) {
        log_peer(LOG_WARNING,
                 "Rejected invalid client IP: %s", local_ip);
        return -1;
    }

    {
        server_client_t *by_addr = server_find_by_addr(from);
        server_client_t *by_ip   = server_find_by_ip(addr.s_addr);

        /* The address may be in use by a live session whose address came
         * from [defaults] or the auto pool, which profile_owner_of_ip()
         * cannot see. Do not hand it to a different keyed client. */
        if (current_config.encrypt && by_ip != NULL && by_ip != by_addr &&
            strcasecmp(by_ip->client_id, client_id) != 0 &&
            keystore_has_owner(by_ip->client_id)) {
            char peer[64];
            net_sockaddr_to_string(from, peer, sizeof(peer));
            log_peer(LOG_WARNING,
                     "Rejected REGISTER from %s: address %s is in use "
                     "by client %s", peer, local_ip, by_ip->client_id);
            return -1;
        }

        if (by_addr != NULL) {
            by_addr->ip_be = addr.s_addr;
            by_addr->last_seen = g_now;
            by_addr->last_register_time = g_now;
            snprintf(by_addr->client_id, sizeof(by_addr->client_id), "%s", client_id);
            if (rstate.live != by_addr) {
                server_begin_session(by_addr, rseq, &rstate);
            }
            by_addr->registered = 1U;
            if (used_key != NULL) {
                memcpy(by_addr->psk_key, used_key, 32);
            }
            return 0;
        }

        if (by_ip != NULL) {
            char old_peer[64], new_peer[64];
            net_sockaddr_to_string(&by_ip->addr, old_peer, sizeof(old_peer));
            net_sockaddr_to_string(from, new_peer, sizeof(new_peer));
            log_message(LOG_INFO, "Client %s reconnected: %s -> %s",
                        local_ip, old_peer, new_peer);
            by_ip->addr = *from;
            by_ip->last_seen = g_now;
            by_ip->last_register_time = g_now;
            snprintf(by_ip->client_id, sizeof(by_ip->client_id), "%s", client_id);
            if (rstate.live != by_ip) {
                server_begin_session(by_ip, rseq, &rstate);
            }
            by_ip->registered = 1U;
            if (used_key != NULL) {
                memcpy(by_ip->psk_key, used_key, 32);
            }
            return 0;
        }

        entry = server_alloc_client();
        if (entry == NULL) {
            log_message(LOG_WARNING, "No free slots for client %s", local_ip);
            return -1;
        }
        entry->ip_be = addr.s_addr;
        entry->addr = *from;
        entry->last_seen = g_now;
        entry->last_register_time = g_now;
        snprintf(entry->client_id, sizeof(entry->client_id), "%s", client_id);
        server_begin_session(entry, rseq, &rstate);
        entry->registered = 1U;
        if (used_key != NULL) {
            memcpy(entry->psk_key, used_key, 32);
        }
        log_message(LOG_INFO, "Registered client %s", local_ip);
        return 0;
    }
}

const uint8_t *server_try_all_keys(const uint8_t *buffer, size_t len,
                                   uint8_t *type,
                                   uint8_t *plain, size_t plain_size,
                                   size_t *plain_len, uint64_t *seq)
{
    size_t i;

    for (i = 0U; i < server_keystore_count; ++i) {
        if (crypto_decrypt_packet(buffer, len, type, plain, plain_size,
                                  plain_len, seq,
                                  server_keystore[i].psk_key) == 0) {
            return server_keystore[i].psk_key;
        }
    }
    return NULL;
}

int server_forward_packet(const uint8_t *packet, size_t len,
                          uint32_t src_ip_be, uint32_t dst_ip_be)
{
    server_client_t *dst;
    uint8_t encrypted[TUN_PACKET_MAX + CAMEX_HDR_LEN + 1 + 16U];
    size_t encrypted_len = 0;

    (void)src_ip_be;

    dst = server_find_by_ip(dst_ip_be);
    if (dst == NULL) {
        if (tun_fd >= 0) {
            return tun_write_packet(packet, len);
        }
        return -1;
    }

    dst->last_seen = (g_now != 0) ? g_now : time(NULL);

    if (current_config.encrypt) {
        uint64_t seq = dst->send_seq;
        if (crypto_encrypt_packet(CAMEX_PACKET_DATA, seq,
                dst->send_nonce_prefix,
                is_zero_key(dst->psk_key) ? NULL : dst->psk_key,
                packet, len, encrypted, sizeof(encrypted),
                &encrypted_len) != 0) {
            return -1;
        }
        dst->send_seq = seq + 1U;
        if (dst->tcp_fd >= 0) {
            return net_tcp_send_frame(dst->tcp_fd, encrypted, encrypted_len);
        }
        return net_send_payload(net_fd, &dst->addr, encrypted, encrypted_len);
    }

    if (dst->tcp_fd >= 0) {
        return net_tcp_send_frame(dst->tcp_fd, packet, len);
    }
    return net_send_payload(net_fd, &dst->addr, packet, len);
}

int server_handle_packet(const uint8_t *buffer, size_t len,
                         const struct sockaddr_in *from, int fd)
{
    uint8_t plain[TUN_PACKET_MAX];
    uint8_t type = 0U;
    size_t plain_len = 0U;
    uint64_t seq = 0;
    uint32_t src_ip_be = 0;
    uint32_t dst_ip_be = 0;
    server_client_t *src;
    char peer[64];

    if (buffer == NULL || from == NULL || len == 0U) {
        return -1;
    }

    if (current_config.encrypt) {
        const uint8_t *used_key = NULL;
        /* Only REGISTER may be authenticated with any keystore key: it is
         * the message that binds a key to a client ID, and it is checked
         * by server_register_allowed(). Every other packet must use the
         * key of the session registered for its source address (#5). The
         * type byte is part of the authenticated header, so a packet
         * cannot pretend to be a REGISTER. */
        int is_register = (len > 4U && buffer[4] == CAMEX_PACKET_REGISTER);

        src = server_find_by_addr(from);
        if (src != NULL && !is_zero_key(src->psk_key) &&
            crypto_decrypt_packet(buffer, len, &type, plain, sizeof(plain),
                                  &plain_len, &seq, src->psk_key) == 0) {
            used_key = src->psk_key;
        } else if (is_register) {
            used_key = server_try_all_keys(buffer, len, &type, plain,
                                           sizeof(plain), &plain_len, &seq);
        }

        if (used_key == NULL) {
            net_sockaddr_to_string(from, peer, sizeof(peer));
            log_peer(LOG_WARNING,
                     "Dropped packet from %s: %s (type=%u, len=%u)", peer,
                     is_register ? "decryption failed with all keys"
                     : "no registered session for this key",
                     (unsigned)(len > 4U ? buffer[4] : 0U), (unsigned)len);
            return -1;
        }

        if (type == CAMEX_PACKET_REGISTER) {
            server_register_seq_t rseq;

            memcpy(rseq.nonce_prefix, buffer + 5, 4);
            rseq.seq = seq;
            rseq.key = used_key;
            return server_handle_plain_register(plain, plain_len, from,
                                                used_key, fd, &rseq);
        }

        /* From here on: authenticated with the session key of src */
        if (type == CAMEX_PACKET_CONFIG) {
            return 0;
        }

        /* Only packets of the client's current session: a packet with an
         * older nonce prefix belongs to an ended session (#7). */
        if (src->has_recv_prefix &&
            memcmp(src->recv_nonce_prefix, buffer + 5, 4) != 0) {
            net_sockaddr_to_string(from, peer, sizeof(peer));
            log_peer(LOG_WARNING,
                     "Dropped packet from %s: not from the client's "
                     "current session", peer);
            return -1;
        }

        if (replay_check(&src->recv_seq_max, &src->recv_window, seq) != 0) {
            net_sockaddr_to_string(from, peer, sizeof(peer));
            log_peer(LOG_WARNING,
                     "Dropped packet from %s: replay check failed "
                     "(seq=%" PRIu64 ", max=%" PRIu64 ")",
                     peer, seq, src->recv_seq_max);
            return -1;
        }

        if (type == CAMEX_PACKET_KEEPALIVE) {
            src->last_seen = g_now;
            server_send_keepalive(src);
            return 0;
        }

        if (type != CAMEX_PACKET_DATA) {
            net_sockaddr_to_string(from, peer, sizeof(peer));
            log_peer(LOG_WARNING,
                     "Dropped packet from %s: unknown type %u",
                     peer, (unsigned)type);
            return -1;
        }

        if (ipv4_parse_endpoints(plain, plain_len,
                                 &src_ip_be, &dst_ip_be) != 0) {
            return 0;
        }

        /* A client may only send from its own tunnel address (#6). */
        if (src->ip_be == 0U || src_ip_be != src->ip_be) {
            char claimed[INET_ADDRSTRLEN];
            struct in_addr a;

            a.s_addr = src_ip_be;
            inet_ntop(AF_INET, &a, claimed, sizeof(claimed));
            net_sockaddr_to_string(from, peer, sizeof(peer));
            log_peer(LOG_WARNING,
                     "Dropped packet from %s (client %s): source address "
                     "%s is not the client's tunnel address",
                     peer, src->client_id, claimed);
            return -1;
        }

        src->last_seen = g_now;
        return server_forward_packet(plain, plain_len, src_ip_be, dst_ip_be);
    }

    /* Plaintext mode: nothing is authenticated */
    if (len >= 4U && memcmp(buffer, CAMEX_MAGIC, 4) == 0) {
        if (len >= 5U && buffer[4] == CAMEX_PACKET_KEEPALIVE) {
            /* Plaintext keepalive: update client last_seen and answer */
            server_client_t *ka = server_find_by_addr(from);
            if (ka != NULL) {
                ka->last_seen = g_now;
                server_send_keepalive(ka);
            }
            return 0;
        }
        if (server_handle_plain_register(buffer, len, from, NULL, fd, NULL) != 0) {
            net_sockaddr_to_string(from, peer, sizeof(peer));
            log_message(LOG_WARNING,
                        "Failed to handle plaintext register from %s",
                        peer);
        }
        return 0;
    }

    if (ipv4_parse_endpoints(buffer, len, &src_ip_be, &dst_ip_be) != 0) {
        return 0;
    }

    src = server_upsert_client(src_ip_be, from);
    if (src == NULL) {
        return -1;
    }

    return server_forward_packet(buffer, len, src_ip_be, dst_ip_be);
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
