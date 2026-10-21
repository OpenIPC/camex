/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * config.c — configuration file parser, server DB, profiles
 *
 */

#include "config.h"
#include "crypto.h"
#include "keys.h"
#include "log.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#ifndef _WIN32
#include <arpa/inet.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>

camex_server_db_t server_db;
camex_keystore_t server_ks;

void config_profile_reset(camex_profile_t *profile)
{
    if (profile == NULL) {
        return;
    }

    memset(profile, 0, sizeof(*profile));
    profile->mtu = 0;
}

int config_profile_append_route(camex_profile_t *profile, const char *value)
{
    if (profile == NULL) {
        return -1;
    }

    return append_route_option(profile->route_cidrs, &profile->route_count,
                               value, "route CIDR");
}

void config_profile_merge(camex_profile_t *dst, const camex_profile_t *src)
{
    size_t i;

    if (dst == NULL || src == NULL) {
        return;
    }

    if (src->local_cidr[0] != '\0') {
        snprintf(dst->local_cidr, sizeof(dst->local_cidr), "%s",
                 src->local_cidr);
    }
    if (src->gateway_ip[0] != '\0') {
        snprintf(dst->gateway_ip, sizeof(dst->gateway_ip), "%s",
                 src->gateway_ip);
    }
    if (src->mtu > 0) {
        dst->mtu = src->mtu;
    }
    for (i = 0; i < src->route_count && dst->route_count < CAMEX_MAX_ROUTES;
         ++i) {
        snprintf(dst->route_cidrs[dst->route_count],
                 sizeof(dst->route_cidrs[0]), "%s", src->route_cidrs[i]);
        ++dst->route_count;
    }
}

void config_server_db_reset(void)
{
    memset(&server_db, 0, sizeof(server_db));
    config_profile_reset(&server_db.defaults);
    server_db.globals.pool_net = CAMEX_AUTO_POOL_DEFAULT_NET;
    server_db.globals.pool_prefix = CAMEX_AUTO_POOL_DEFAULT_PREFIX;
}

void config_db_free(camex_server_db_t *db)
{
    if (db == NULL) {
        return;
    }
    if (db->clients != NULL) {
        crypto_wipe(db->clients, db->client_cap * sizeof(*db->clients));
        free(db->clients);
    }
    slotidx_free(&db->by_id);
    slotidx_free(&db->by_ip);
    crypto_wipe(db, sizeof(*db));
}

void config_keystore_free(camex_keystore_t *ks)
{
    if (ks == NULL) {
        return;
    }
    if (ks->entries != NULL) {
        crypto_wipe(ks->entries, ks->count * sizeof(*ks->entries));
        free(ks->entries);
    }
    slotidx_free(&ks->by_owner);
    slotidx_free(&ks->by_key);
    memset(ks, 0, sizeof(*ks));
}

static int profile_id_match(uint32_t slot, const void *key)
{
    return slot < server_db.client_count &&
           server_db.clients[slot].active &&
           strcasecmp(server_db.clients[slot].client_id,
                      (const char *)key) == 0;
}

camex_server_profile_t *config_db_find_client(const char *client_id)
{
    long i;

    if (client_id == NULL || *client_id == '\0') {
        return NULL;
    }

    i = slotidx_find(&server_db.by_id, slotidx_hash_nocase(client_id),
                     profile_id_match, client_id);
    return i >= 0 ? &server_db.clients[i] : NULL;
}

static int profile_ip_match(uint32_t slot, const void *key)
{
    return slot < server_db.client_count &&
           server_db.clients[slot].active &&
           server_db.clients[slot].has_ip &&
           server_db.clients[slot].ip_be == *(const uint32_t *)key;
}

camex_server_profile_t *config_db_find_profile_by_ip(uint32_t ip_be)
{
    long i = slotidx_find(&server_db.by_ip, slotidx_hash_u32(ip_be),
                          profile_ip_match, &ip_be);

    return i >= 0 ? &server_db.clients[i] : NULL;
}

camex_server_profile_t *config_db_get_client_slot(const char *client_id)
{
    camex_server_profile_t *slot = NULL;
    size_t id_len;

    if (client_id == NULL || *client_id == '\0') {
        return NULL;
    }

    slot = config_db_find_client(client_id);
    if (slot != NULL) {
        return slot;
    }

    id_len = strlen(client_id);
    if (id_len >= sizeof(slot->client_id)) {
        log_message(LOG_ERR, "Client ID too long: %s", client_id);
        return NULL;
    }
    if (server_db.client_count >= CAMEX_MAX_CLIENTS_LIMIT) {
        log_message(LOG_ERR, "More than %u [client] sections",
                    (unsigned)CAMEX_MAX_CLIENTS_LIMIT);
        return NULL;
    }
    if (server_db.client_count == server_db.client_cap) {
        size_t cap = server_db.client_cap != 0U ? server_db.client_cap * 2U
                                                : 16U;
        camex_server_profile_t *grown =
            key_locked_calloc(cap, sizeof(*grown), "[client] profiles");

        if (grown == NULL) {
            log_message(LOG_ERR, "Out of memory for client profiles");
            return NULL;
        }
        if (server_db.clients != NULL) {
            /* Not realloc(): the old block holds psk= texts to wipe */
            memcpy(grown, server_db.clients,
                   server_db.client_count * sizeof(*grown));
            crypto_wipe(server_db.clients,
                        server_db.client_cap * sizeof(*grown));
            free(server_db.clients);
        }
        server_db.clients = grown;
        server_db.client_cap = cap;
    }
    if (slotidx_insert(&server_db.by_id, slotidx_hash_nocase(client_id),
                       (uint32_t)server_db.client_count) != 0) {
        log_message(LOG_ERR, "Out of memory for client profiles");
        return NULL;
    }

    slot = &server_db.clients[server_db.client_count++];
    memset(slot, 0, sizeof(*slot));
    slot->active = 1U;
    memcpy(slot->client_id, client_id, id_len + 1U);
    config_profile_reset(&slot->profile);
    return slot;
}

int config_parse_pool(const char *value, uint32_t *net, uint8_t *prefix)
{
    char addr[16];
    const char *slash;
    char *end = NULL;
    unsigned long p;
    struct in_addr a;
    uint32_t mask;
    uint32_t host;

    slash = value != NULL ? strchr(value, '/') : NULL;
    if (slash == NULL || (size_t)(slash - value) >= sizeof(addr)) {
        log_message(LOG_ERR, "Invalid auto_pool: %s (network/prefix, "
                    "e.g. 10.200.0.0/24)", value != NULL ? value : "");
        return -1;
    }
    memcpy(addr, value, (size_t)(slash - value));
    addr[slash - value] = '\0';
    errno = 0;
    p = strtoul(slash + 1, &end, 10);
    if (inet_pton(AF_INET, addr, &a) != 1 || slash[1] == '\0' ||
        end == NULL || *end != '\0' || errno != 0 ||
        p < CAMEX_AUTO_POOL_MIN_PREFIX || p > CAMEX_AUTO_POOL_MAX_PREFIX) {
        log_message(LOG_ERR, "Invalid auto_pool: %s (network/prefix with "
                    "a prefix of /%u to /%u)", value,
                    (unsigned)CAMEX_AUTO_POOL_MIN_PREFIX,
                    (unsigned)CAMEX_AUTO_POOL_MAX_PREFIX);
        return -1;
    }
    mask = 0xFFFFFFFFU << (32U - (unsigned)p);
    host = ntohl(a.s_addr);
    if ((host & ~mask) != 0U) {
        log_message(LOG_ERR, "Invalid auto_pool: %s has host bits set "
                    "(the network is %u.%u.%u.%u/%lu)", value,
                    (unsigned)((host & mask) >> 24),
                    (unsigned)(((host & mask) >> 16) & 0xFFU),
                    (unsigned)(((host & mask) >> 8) & 0xFFU),
                    (unsigned)((host & mask) & 0xFFU), p);
        return -1;
    }
    *net = host;
    *prefix = (uint8_t)p;
    return 0;
}

static int parse_max_clients(const char *value, unsigned int *out)
{
    char *end = NULL;
    unsigned long v;

    errno = 0;
    v = strtoul(value, &end, 10);
    if (value[0] == '\0' || value[0] == '-' || end == NULL ||
        *end != '\0' || errno != 0 || v < 1UL ||
        v > (unsigned long)CAMEX_MAX_CLIENTS_LIMIT) {
        log_message(LOG_ERR, "Invalid max_clients: %s (1 to %u)", value,
                    (unsigned)CAMEX_MAX_CLIENTS_LIMIT);
        return -1;
    }
    *out = (unsigned int)v;
    return 0;
}

/* After the file is read: the address of each local_cidr, indexed */
static int config_db_index_ips(void)
{
    size_t i;

    if (slotidx_reset(&server_db.by_ip, server_db.client_count) != 0) {
        log_message(LOG_ERR, "Out of memory for client profiles");
        return -1;
    }
    for (i = 0U; i < server_db.client_count; ++i) {
        camex_server_profile_t *c = &server_db.clients[i];
        char ip[16];
        char mask[16];
        struct in_addr a;

        if (!c->active || c->profile.local_cidr[0] == '\0' ||
            parse_local_cidr(c->profile.local_cidr, ip, sizeof(ip),
                             mask, sizeof(mask)) != 0 ||
            inet_pton(AF_INET, ip, &a) != 1) {
            continue;
        }
        c->has_ip = 1U;
        c->ip_be = a.s_addr;
        if (slotidx_insert(&server_db.by_ip, slotidx_hash_u32(a.s_addr),
                           (uint32_t)i) != 0) {
            log_message(LOG_ERR, "Out of memory for client profiles");
            return -1;
        }
    }
    return 0;
}

int config_db_apply_kv(camex_profile_t *profile,
                       const char *key, const char *value)
{
    if (profile == NULL || key == NULL || value == NULL) {
        return -1;
    }

    if (strcmp(key, "local_cidr") == 0) {
        return copy_option(profile->local_cidr, sizeof(profile->local_cidr),
                           value, "local CIDR");
    }
    if (strcmp(key, "gateway_ip") == 0) {
        return copy_option(profile->gateway_ip, sizeof(profile->gateway_ip),
                           value, "gateway IP");
    }
    if (strcmp(key, "route_cidr") == 0) {
        return config_profile_append_route(profile, value);
    }
    if (strcmp(key, "mtu") == 0) {
        return parse_mtu(value, &profile->mtu);
    }
    if (strcmp(key, "psk") == 0) {
        return copy_option(profile->psk, sizeof(profile->psk),
                           value, "per-client PSK");
    }

    log_message(LOG_WARNING, "Ignoring unknown config key: %s", key);
    return 0;
}

/* The parser proper; line is the caller's buffer, wiped by the caller
 * on every exit (it can hold psk= and server_key= text) */
static int config_db_parse_file(const char *path, char *line,
                                size_t line_size)
{
    FILE *fp;
    camex_profile_t *current = NULL;
    camex_server_profile_t *client = NULL;
    enum {
        SECTION_NONE = 0,
        SECTION_DEFAULTS,
        SECTION_CLIENT,
        SECTION_SERVER
    } section = SECTION_NONE;

    config_server_db_reset();

    if (path == NULL || *path == '\0') {
        return 0;
    }

    fp = fopen(path, "r");
    if (fp == NULL) {
        if (errno == ENOENT) {
            log_message(LOG_WARNING, "Config file not found: %s", path);
            return 0;
        }
        print_errno_message(LOG_ERR, "fopen(config)");
        return -1;
    }

    while (fgets(line, (int)line_size, fp) != NULL) {
        char *content;
        char *equals;
        trim_whitespace(line);
        if (*line == '\0' || *line == '#' || *line == ';') {
            continue;
        }

        if (line[0] == '[') {
            size_t len = strlen(line);

            if (len < 2U || line[len - 1U] != ']') {
                log_message(LOG_ERR, "Invalid config section: %s", line);
                fclose(fp);
                return -1;
            }

            line[len - 1U] = '\0';
            content = line + 1;
            trim_whitespace(content);

            if (strcasecmp(content, "server") == 0) {
                section = SECTION_SERVER;
                current = NULL;
                client = NULL;
                continue;
            }

            if (strcasecmp(content, "defaults") == 0) {
                section = SECTION_DEFAULTS;
                current = &server_db.defaults;
                client = NULL;
                continue;
            }

            if (strncasecmp(content, "client", 6) == 0 &&
                (content[6] == '\0' || isspace((unsigned char)content[6]))) {
                char *client_id_str = content + 6;

                trim_whitespace(client_id_str);
                if (*client_id_str == '\0') {
                    log_message(LOG_ERR, "Missing client ID in section: %s",
                                line);
                    fclose(fp);
                    return -1;
                }

                client = config_db_get_client_slot(client_id_str);
                if (client == NULL) {
                    log_message(LOG_ERR,
                                "No free slots for client profile: %s",
                                client_id_str);
                    fclose(fp);
                    return -1;
                }

                section = SECTION_CLIENT;
                current = &client->profile;
                continue;
            }

            log_message(LOG_ERR, "Unknown config section: %s", content);
            fclose(fp);
            return -1;
        }

        equals = strchr(line, '=');
        if (equals == NULL) {
            log_message(LOG_ERR, "Invalid config line: %s", line);
            fclose(fp);
            return -1;
        }

        *equals = '\0';
        content = line;
        trim_whitespace(content);
        trim_whitespace(equals + 1);

        if (*content == '\0' || *(equals + 1) == '\0') {
            log_message(LOG_ERR, "Invalid config line: %s=%s",
                        content, equals + 1);
            fclose(fp);
            return -1;
        }

        if (section == SECTION_SERVER) {
            camex_server_globals_t *g = &server_db.globals;
            const char *val = equals + 1;
            int rc = 0;

            if (strcmp(content, "port") == 0) {
                rc = parse_port(val, &g->port);
            } else if (strcmp(content, "bind_ip") == 0) {
                rc = copy_option(g->bind_ip, sizeof(g->bind_ip),
                                 val, "bind IP");
            } else if (strcmp(content, "local_cidr") == 0) {
                rc = copy_option(g->local_cidr, sizeof(g->local_cidr),
                                 val, "local CIDR");
            } else if (strcmp(content, "mtu") == 0) {
                rc = parse_mtu(val, &g->mtu);
            } else if (strcmp(content, "encrypt") == 0) {
                if (strcasecmp(val, "yes") == 0 ||
                    strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0) {
                    g->encrypt = 1U;
                } else if (strcasecmp(val, "no") == 0 ||
                           strcasecmp(val, "false") == 0 ||
                           strcmp(val, "0") == 0) {
                    g->encrypt = 0U;
                } else {
                    log_message(LOG_ERR, "Invalid value for encrypt: %s",
                                val);
                    rc = -1;
                }
                g->encrypt_set = 1U;
            } else if (strcmp(content, "allow_unknown_clients") == 0) {
                if (strcasecmp(val, "yes") == 0 ||
                    strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0) {
                    g->deny_unknown_clients = 0U;
                } else if (strcasecmp(val, "no") == 0 ||
                           strcasecmp(val, "false") == 0 ||
                           strcmp(val, "0") == 0) {
                    g->deny_unknown_clients = 1U;
                } else {
                    log_message(LOG_ERR,
                                "Invalid value for allow_unknown_clients: %s",
                                val);
                    rc = -1;
                }
            } else if (strcmp(content, "server_key") == 0) {
                rc = copy_option(g->server_key, sizeof(g->server_key), val,
                                 "server key");
            } else if (strcmp(content, "psk") == 0) {
                rc = copy_option(g->psk, sizeof(g->psk), val, "PSK");
            } else if (strcmp(content, "auto_pool") == 0) {
                rc = config_parse_pool(val, &g->pool_net, &g->pool_prefix);
                g->pool_set = rc == 0 ? 1U : 0U;
            } else if (strcmp(content, "max_clients") == 0) {
                rc = parse_max_clients(val, &g->max_clients);
            } else if (strcmp(content, "tun_dev") == 0) {
                rc = copy_option(g->tun_dev, sizeof(g->tun_dev),
                                 val, "TUN device");
            } else if (strcmp(content, "bind_dev") == 0) {
                rc = copy_option(g->bind_dev, sizeof(g->bind_dev),
                                 val, "bind device");
            } else if (strcmp(content, "pid_file") == 0) {
                rc = copy_option(g->pid_file, sizeof(g->pid_file),
                                 val, "PID file");
            } else {
                log_message(LOG_WARNING,
                            "Ignoring unknown [server] key: %s", content);
            }

            if (rc != 0) {
                fclose(fp);
                return -1;
            }
            continue;
        }

        if (current == NULL) {
            log_message(LOG_ERR,
                        "Config value outside of a section: %s", line);
            fclose(fp);
            return -1;
        }

        if (config_db_apply_kv(current, content, equals + 1) != 0) {
            fclose(fp);
            return -1;
        }
    }

    fclose(fp);
    if (config_db_index_ips() != 0) {
        return -1;
    }
    server_db.loaded = 1U;
    (void)section;
    return 0;
}

int config_db_load_file(const char *path)
{
    char line[512];
    int rc = config_db_parse_file(path, line, sizeof(line));

    crypto_wipe(line, sizeof(line));
    return rc;
}

int config_db_lookup_profile(const char *client_id, camex_profile_t *profile)
{
    camex_server_profile_t *client;

    if (profile == NULL) {
        return -1;
    }

    *profile = server_db.defaults;
    client = config_db_find_client(client_id);
    if (client != NULL) {
        config_profile_merge(profile, &client->profile);
    }

    return 0;
}

/* Key store lookups (#5): by key (32 bytes) and by owner (client ID) */
typedef struct {
    const uint8_t *key;
    const char *owner;    /* NULL = any owner */
    size_t below;         /* only entries before this one */
} ks_query_t;

static int ks_key_match(uint32_t slot, const void *key)
{
    const ks_query_t *q = key;
    const camex_keystore_entry_t *e;

    if (slot >= server_ks.count || slot >= q->below) {
        return 0;
    }
    e = &server_ks.entries[slot];
    return !e->locked && memcmp(e->psk_key, q->key, 32) == 0 &&
           (q->owner == NULL || strcasecmp(e->owner, q->owner) == 0);
}

static int ks_owner_match(uint32_t slot, const void *key)
{
    const ks_query_t *q = key;
    const camex_keystore_entry_t *e;

    if (slot >= server_ks.count) {
        return 0;
    }
    e = &server_ks.entries[slot];
    return e->owner[0] != '\0' && strcasecmp(e->owner, q->owner) == 0 &&
           (q->key == NULL ||
            (!e->locked && memcmp(e->psk_key, q->key, 32) == 0));
}

static long ks_find_key(const uint8_t *key, const char *owner, size_t below)
{
    ks_query_t q;

    q.key = key;
    q.owner = owner;
    q.below = below;
    return slotidx_find(&server_ks.by_key, slotidx_hash_bytes(key, 32),
                        ks_key_match, &q);
}

const char *config_ks_owner(const uint8_t *key)
{
    long i;

    if (key == NULL) {
        return "";
    }
    /* The first entry with this key, as a scan would find it: the global
     * key comes first */
    i = ks_find_key(key, NULL, (size_t)-1);
    return i >= 0 ? server_ks.entries[i].owner : "";
}

int config_ks_has_owner(const char *client_id)
{
    ks_query_t q;

    if (client_id == NULL || client_id[0] == '\0') {
        return 0;
    }
    q.key = NULL;
    q.owner = client_id;
    q.below = (size_t)-1;
    return slotidx_find(&server_ks.by_owner, slotidx_hash_nocase(client_id),
                        ks_owner_match, &q) >= 0;
}

int config_ks_key_owned_by(const uint8_t *key, const char *client_id)
{
    ks_query_t q;

    if (key == NULL || client_id == NULL || client_id[0] == '\0') {
        return 0;
    }
    q.key = key;
    q.owner = client_id;
    q.below = (size_t)-1;
    return slotidx_find(&server_ks.by_owner, slotidx_hash_nocase(client_id),
                        ks_owner_match, &q) >= 0;
}

int config_ks_contains(const uint8_t *key)
{
    return key != NULL && ks_find_key(key, NULL, (size_t)-1) >= 0;
}

int config_ks_is_global(const uint8_t *key)
{
    return key != NULL && ks_find_key(key, "", (size_t)-1) >= 0;
}

/* Wipe the psk= texts of the profiles: the keystore holds the keys */
static void wipe_profile_psks(void)
{
    size_t i;

    for (i = 0U; i < server_db.client_count; ++i) {
        crypto_wipe(server_db.clients[i].profile.psk,
                    sizeof(server_db.clients[i].profile.psk));
    }
    crypto_wipe(server_db.defaults.psk, sizeof(server_db.defaults.psk));
}

/* server_ks is overwritten: the caller has freed it or keeps it (a reload
 * that may roll back) */
int config_build_keystore(void)
{
    const camex_crypto_t *ctx;
    camex_keystore_t *ks = &server_ks;
    size_t i;
    size_t cap;
    int rc;

    memset(ks, 0, sizeof(*ks));

    ctx = crypto_get_ctx();
    if (!ctx->ready) {
        /* No encryption: the keys are not used, do not derive them */
        wipe_profile_psks();
        return 0;
    }

    cap = server_db.client_count + 1U;
    ks->entries = key_locked_calloc(cap, sizeof(*ks->entries), "the keystore");
    if (ks->entries == NULL || slotidx_reset(&ks->by_key, cap) != 0 ||
        slotidx_reset(&ks->by_owner, cap) != 0) {
        log_message(LOG_ERR, "Out of memory for the keystore");
        config_keystore_free(ks);
        wipe_profile_psks();
        return KEY_ERR_NOMEM;
    }

    if (ctx->has_global) {
        memcpy(ks->entries[0].psk_key, ctx->psk_key, 32);
        ks->count = 1U;
    }

    for (i = 0U; i < server_db.client_count; ++i) {
        camex_keystore_entry_t *e = &ks->entries[ks->count];

        if (!server_db.clients[i].active) {
            continue;
        }
        if (server_db.clients[i].profile.psk[0] == '\0') {
            continue;
        }
        rc = derive_psk_key(server_db.clients[i].profile.psk, e->psk_key);
        if (rc == KEY_ERR_NOMEM) {
            /* Not the key's fault: the caller keeps what it had */
            log_message(LOG_ERR, "Out of memory deriving the key of "
                        "client %s", server_db.clients[i].client_id);
            config_keystore_free(ks);
            wipe_profile_psks();
            return KEY_ERR_NOMEM;
        }
        if (rc != 0) {
            /* The client keeps a locked owner entry that matches no key:
             * it cannot register until its psk= is fixed, and the global
             * key cannot take its ID over either */
            log_message(LOG_ERR, "Invalid psk= for client %s: it cannot "
                        "register until the key is fixed",
                        server_db.clients[i].client_id);
            crypto_wipe(e->psk_key, 32);
            e->locked = 1U;
        }
        snprintf(e->owner, sizeof(e->owner), "%s",
                 server_db.clients[i].client_id);
        ks->count++;
    }

    wipe_profile_psks();

    for (i = 0U; i < ks->count; ++i) {
        /* A locked entry matches no key: only its owner is indexed (its
         * wiped key would pile all of them into one probe chain) */
        if ((!ks->entries[i].locked &&
             slotidx_insert(&ks->by_key,
                            slotidx_hash_bytes(ks->entries[i].psk_key, 32),
                            (uint32_t)i) != 0) ||
            (ks->entries[i].owner[0] != '\0' &&
             slotidx_insert(&ks->by_owner,
                            slotidx_hash_nocase(ks->entries[i].owner),
                            (uint32_t)i) != 0)) {
            log_message(LOG_ERR, "Out of memory for the keystore");
            config_keystore_free(ks);
            return KEY_ERR_NOMEM;
        }
    }

    /* Equal keys are allowed (a client's psk may equal the global one),
     * but they let either holder register as the other: say so. */
    for (i = 1U; i < ks->count; ++i) {
        long j;

        if (ks->entries[i].locked) {
            continue;
        }
        j = ks_find_key(ks->entries[i].psk_key, NULL, i);
        if (j >= 0) {
            log_message(LOG_WARNING,
                        "Client %s has the same psk as %s",
                        ks->entries[i].owner,
                        ks->entries[j].owner[0] != '\0'
                            ? ks->entries[j].owner
                            : "the global --psk");
        }
    }

    {
        size_t usable = 0U;

        for (i = 0U; i < ks->count; ++i) {
            if (!ks->entries[i].locked) {
                ++usable;
            }
        }
        /* Without a key no client can ever connect: say so instead of
         * dropping every handshake */
        if (usable == 0U) {
            log_message(LOG_ERR, "No usable PSK: give --psk, psk= in "
                        "[server], or psk= for each client");
            return -1;
        }
    }
    log_message(LOG_INFO, "Server keystore: %u PSK(s) loaded",
                (unsigned)ks->count);
    return 0;
}

int config_apply_profile(camex_config_t *config, const camex_profile_t *profile)
{
    size_t i;

    if (config == NULL || profile == NULL) {
        return -1;
    }

    if (profile->local_cidr[0] == '\0' || profile->gateway_ip[0] == '\0') {
        return -1;
    }

    snprintf(config->local_cidr, sizeof(config->local_cidr), "%s",
             profile->local_cidr);
    snprintf(config->gateway_ip, sizeof(config->gateway_ip), "%s",
             profile->gateway_ip);
    config->route_count = 0U;
    for (i = 0; i < profile->route_count && i < CAMEX_MAX_ROUTES; ++i) {
        snprintf(config->route_cidrs[config->route_count],
                 sizeof(config->route_cidrs[0]), "%s",
                 profile->route_cidrs[i]);
        ++config->route_count;
    }
    if (profile->mtu > 0) {
        config->mtu = profile->mtu;
    }

    return 0;
}
