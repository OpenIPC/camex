/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * config.c — configuration file parser, server DB, profiles
 *
 */

#include "config.h"
#include "proto.h"
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
    if (src->peer_port != 0) {
        dst->peer_port = src->peer_port;
    }
    if (src->web_port != 0) {
        dst->web_port = src->web_port;
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
    server_db.globals.net_supernet = CAMEX_NET_SUPERNET_DEFAULT_NET;
    server_db.globals.net_super_prefix = CAMEX_NET_SUPERNET_DEFAULT_PREFIX;
    server_db.globals.net_pool_prefix = CAMEX_NET_POOL_PREFIX_DEFAULT;
}

int config_networks_enabled(void)
{
    return server_db.net_count > 0U || server_db.globals.allow_new_networks;
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
    if (db->nets != NULL) {
        crypto_wipe(db->nets, db->net_cap * sizeof(*db->nets));
        free(db->nets);
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
    return config_parse_cidr_key(value, "auto_pool", net, prefix);
}

int config_parse_cidr_key(const char *value, const char *what,
                          uint32_t *net, uint8_t *prefix)
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
        log_message(LOG_ERR, "Invalid %s: %s (network/prefix, "
                    "e.g. 10.200.0.0/24)", what, value != NULL ? value : "");
        return -1;
    }
    memcpy(addr, value, (size_t)(slash - value));
    addr[slash - value] = '\0';
    errno = 0;
    p = strtoul(slash + 1, &end, 10);
    if (inet_pton(AF_INET, addr, &a) != 1 || slash[1] == '\0' ||
        end == NULL || *end != '\0' || errno != 0 ||
        p < CAMEX_AUTO_POOL_MIN_PREFIX || p > CAMEX_AUTO_POOL_MAX_PREFIX) {
        log_message(LOG_ERR, "Invalid %s: %s (network/prefix with "
                    "a prefix of /%u to /%u)", what, value,
                    (unsigned)CAMEX_AUTO_POOL_MIN_PREFIX,
                    (unsigned)CAMEX_AUTO_POOL_MAX_PREFIX);
        return -1;
    }
    mask = 0xFFFFFFFFU << (32U - (unsigned)p);
    host = ntohl(a.s_addr);
    if ((host & ~mask) != 0U) {
        log_message(LOG_ERR, "Invalid %s: %s has host bits set "
                    "(the network is %u.%u.%u.%u/%lu)", what, value,
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

/* client_timeout= (#143): seconds of silence before a client expires */
static int parse_client_timeout(const char *value, unsigned int *out)
{
    char *end = NULL;
    unsigned long v;

    errno = 0;
    v = strtoul(value, &end, 10);
    if (value[0] == '\0' || value[0] == '-' || end == NULL ||
        *end != '\0' || errno != 0 || v < CAMEX_CLIENT_TIMEOUT_MIN ||
        v > CAMEX_CLIENT_TIMEOUT) {
        log_message(LOG_ERR, "Invalid client_timeout: %s (%d to %d seconds)",
                    value, CAMEX_CLIENT_TIMEOUT_MIN, CAMEX_CLIENT_TIMEOUT);
        return -1;
    }
    *out = (unsigned int)v;
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

/* web_port= (#174): 1..65535, digits only */
static int parse_web_port(const char *value, int *out)
{
    char *end = NULL;
    long v;

    errno = 0;
    v = strtol(value, &end, 10);
    if (strspn(value, "0123456789") != strlen(value) || strlen(value) > 5U ||
        errno != 0 || end == value || *end != '\0' || v < 1L ||
        v > 65535L) {
        log_message(LOG_ERR, "Invalid web_port: %s (1 to 65535)", value);
        return -1;
    }
    *out = (int)v;
    return 0;
}

/* peer_port= (#119): 1..65535, digits only, or "none" (-1) */
static int parse_peer_port(const char *value, int *out)
{
    char *end = NULL;
    long v;

    if (strcmp(value, "none") == 0) {
        *out = -1;
        return 0;
    }
    errno = 0;
    v = strtol(value, &end, 10);
    if (strspn(value, "0123456789") != strlen(value) || strlen(value) > 5U ||
        errno != 0 || end == value || *end != '\0' || v < 1L ||
        v > 65535L) {
        log_message(LOG_ERR, "Invalid peer_port: %s (1 to 65535, or none)",
                    value);
        return -1;
    }
    *out = (int)v;
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
    if (strcmp(key, "web_port") == 0) {
        return parse_web_port(value, &profile->web_port);
    }
    if (strcmp(key, "peer_port") == 0) {
        return parse_peer_port(value, &profile->peer_port);
    }

    log_message(LOG_WARNING, "Ignoring unknown config key: %s", key);
    return 0;
}

/* The parser proper; line is the caller's buffer, wiped by the caller
 * on every exit (it can hold psk= and server_key= text) */
/* yes/true/1 or no/false/0; -1 otherwise */
static int parse_yes_no(const char *val)
{
    if (strcasecmp(val, "yes") == 0 || strcasecmp(val, "true") == 0 ||
        strcmp(val, "1") == 0) {
        return 1;
    }
    if (strcasecmp(val, "no") == 0 || strcasecmp(val, "false") == 0 ||
        strcmp(val, "0") == 0) {
        return 0;
    }
    return -1;
}

/* A new [network NAME] block; NULL with a logged reason */
static camex_net_conf_t *config_db_add_network(const char *name)
{
    camex_net_conf_t *n;
    uint8_t id[CAMEX_NET_ID_LEN];
    size_t i;

    if (!netw_name_valid(name)) {
        log_message(LOG_ERR, "Invalid network name: 1 to %u bytes without "
                    "control characters", (unsigned)CAMEX_NET_NAME_MAX);
        return NULL;
    }
    netw_id_from_name(name, id);
    for (i = 0U; i < server_db.net_count; ++i) {
        if (memcmp(server_db.nets[i].id, id, sizeof(id)) == 0) {
            log_message(LOG_ERR, "A [network] block is given twice");
            return NULL;
        }
    }
    if (server_db.net_count >= CAMEX_NET_MAX) {
        log_message(LOG_ERR, "More than %u [network] blocks",
                    (unsigned)CAMEX_NET_MAX);
        return NULL;
    }
    if (server_db.net_count == server_db.net_cap) {
        size_t cap = server_db.net_cap != 0U ? server_db.net_cap * 2U : 8U;
        camex_net_conf_t *grown = calloc(cap, sizeof(*grown));

        if (grown == NULL) {
            log_message(LOG_ERR, "Out of memory for [network] blocks");
            return NULL;
        }
        if (server_db.nets != NULL) {
            memcpy(grown, server_db.nets,
                   server_db.net_count * sizeof(*grown));
            crypto_wipe(server_db.nets, server_db.net_cap * sizeof(*grown));
            free(server_db.nets);
        }
        server_db.nets = grown;
        server_db.net_cap = cap;
    }
    n = &server_db.nets[server_db.net_count++];
    memset(n, 0, sizeof(*n));
    memcpy(n->id, id, sizeof(id));
    /* Shown by its name unless label= says otherwise */
    netw_label_from_name(name, n->label, sizeof(n->label));
    n->isolation = -1;
    n->name_len = (uint8_t)(strlen(name) > 255U ? 255U : strlen(name));
    crypto_wipe(id, sizeof(id));
    return n;
}

/* A short network name can be guessed one CPace attempt at a time: a
 * [network] block takes one only with weak_name = yes. The name is a
 * secret: the messages give the label */
static int config_db_check_names(void)
{
    size_t i;

    for (i = 0U; i < server_db.net_count; ++i) {
        const camex_net_conf_t *n = &server_db.nets[i];
        char label[CAMEX_NET_LABEL_MAX + 1U];

        if (n->name_len >= CAMEX_NET_NAME_MIN) {
            continue;
        }
        /* By its ID: without label= the label is the name itself */
        netw_default_label(n->id, label, sizeof(label));
        if (!n->weak_name) {
            log_message(LOG_ERR, "Network %s: its name has %u characters, "
                        "fewer than %u, and could be guessed; choose a "
                        "longer one, or set weak_name = yes in its block",
                        label, (unsigned)n->name_len,
                        (unsigned)CAMEX_NET_NAME_MIN);
            return -1;
        }
        log_message(LOG_WARNING, "Network %s: its name has %u characters "
                    "(weak_name = yes): it could be guessed",
                    label, (unsigned)n->name_len);
    }
    return 0;
}

static int config_db_parse_file(const char *path, char *line,
                                size_t line_size)
{
    FILE *fp;
    camex_profile_t *current = NULL;
    camex_server_profile_t *client = NULL;
    camex_net_conf_t *network = NULL;
    unsigned long lineno = 0UL;   /* for the errors of a value */
    enum {
        SECTION_NONE = 0,
        SECTION_DEFAULTS,
        SECTION_CLIENT,
        SECTION_SERVER,
        SECTION_NETWORK
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

        ++lineno;
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

            if (strncasecmp(content, "network", 7) == 0 &&
                (content[7] == '\0' || isspace((unsigned char)content[7]))) {
                char *name = content + 7;

                trim_whitespace(name);
                network = config_db_add_network(name);
                if (network == NULL) {
                    log_message(LOG_ERR, "%s, line %lu: [network]", path,
                                lineno);
                    fclose(fp);
                    return -1;
                }
                section = SECTION_NETWORK;
                current = NULL;
                client = NULL;
                continue;
            }

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
            } else if (strcmp(content, "client_isolation") == 0) {
                if (strcasecmp(val, "yes") == 0 ||
                    strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0) {
                    g->client_isolation = 1U;
                } else if (strcasecmp(val, "no") == 0 ||
                           strcasecmp(val, "false") == 0 ||
                           strcmp(val, "0") == 0) {
                    g->client_isolation = 0U;
                } else {
                    log_message(LOG_ERR,
                                "Invalid value for client_isolation: %s",
                                val);
                    rc = -1;
                }
            } else if (strcmp(content, "allow_new_networks") == 0) {
                int yn = parse_yes_no(val);

                if (yn < 0) {
                    log_message(LOG_ERR,
                                "Invalid value for allow_new_networks: %s",
                                val);
                    rc = -1;
                } else {
                    g->allow_new_networks = (uint8_t)yn;
                }
            } else if (strcmp(content, "cpace_per_ip") == 0 ||
                       strcmp(content, "cpace_total") == 0 ||
                       strcmp(content, "cpace_ban_after") == 0 ||
                       strcmp(content, "cpace_ban_time") == 0) {
                char *end = NULL;
                unsigned long v = strtoul(val, &end, 10);

                if (end == val || *end != '\0' || v > 1000000UL) {
                    log_message(LOG_ERR, "Invalid %s: %s (0 to 1000000, 0 "
                                "turns it off)", content, val);
                    rc = -1;
                } else if (strcmp(content, "cpace_per_ip") == 0) {
                    g->cpace_per_ip = (unsigned int)v;
                    g->cpace_per_ip_set = 1U;
                } else if (strcmp(content, "cpace_total") == 0) {
                    g->cpace_total = (unsigned int)v;
                    g->cpace_total_set = 1U;
                } else if (strcmp(content, "cpace_ban_after") == 0) {
                    g->cpace_ban_after = (unsigned int)v;
                    g->cpace_ban_after_set = 1U;
                } else {
                    g->cpace_ban_time = (unsigned int)v;
                    g->cpace_ban_time_set = 1U;
                }
            } else if (strcmp(content, "network_supernet") == 0) {
                rc = config_parse_cidr_key(val, "network_supernet",
                                           &g->net_supernet,
                                           &g->net_super_prefix);
            } else if (strcmp(content, "network_prefix") == 0) {
                char *end = NULL;
                unsigned long p = strtoul(val, &end, 10);

                if (end == val || *end != '\0' || p < 9UL || p > 30UL) {
                    log_message(LOG_ERR, "Invalid network_prefix: %s (9 to "
                                "30)", val);
                    rc = -1;
                } else {
                    g->net_pool_prefix = (uint8_t)p;
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
            } else if (strcmp(content, "client_timeout") == 0) {
                rc = parse_client_timeout(val, &g->client_timeout);
            } else if (strcmp(content, "tun_dev") == 0) {
                rc = copy_option(g->tun_dev, sizeof(g->tun_dev),
                                 val, "TUN device");
            } else if (strcmp(content, "bind_dev") == 0) {
                rc = copy_option(g->bind_dev, sizeof(g->bind_dev),
                                 val, "bind device");
            } else if (strcmp(content, "pid_file") == 0) {
                rc = copy_option(g->pid_file, sizeof(g->pid_file),
                                 val, "PID file");
            } else if (strcmp(content, "control_socket") == 0) {
                rc = copy_option(g->control_socket, sizeof(g->control_socket),
                                 val, "control socket");
            } else if (strcmp(content, "domain") == 0) {
                /* The domain of the nginx map and start page (#106) */
                if (!camex_domain_valid(val)) {
                    log_message(LOG_ERR, "Invalid domain=: %s (host name "
                                "labels: letters, digits and '-', separated "
                                "by dots)", val);
                    rc = -1;
                } else {
                    rc = copy_option(g->domain, sizeof(g->domain), val,
                                     "domain");
                }
            } else if (strcmp(content, "name") == 0) {
                /* A client's display name (#105), as in [defaults] */
                log_message(LOG_ERR, "name= belongs in a [client ID] "
                            "block");
                rc = -1;
            } else if (strcmp(content, "web_port") == 0) {
                /* nginx's port on a client (#174): per client */
                log_message(LOG_ERR, "web_port= belongs in [defaults] or "
                            "a [client ID] block");
                rc = -1;
            } else if (strcmp(content, "peer_port") == 0) {
                /* The start page's peer link (#119): per client */
                log_message(LOG_ERR, "peer_port= belongs in [defaults] or "
                            "a [client ID] block");
                rc = -1;
            } else {
                log_message(LOG_WARNING,
                            "Ignoring unknown [server] key: %s", content);
            }

            if (rc != 0) {
                log_message(LOG_ERR, "%s, line %lu: [server] %s=", path,
                            lineno, content);
                fclose(fp);
                return -1;
            }
            continue;
        }

        if (section == SECTION_NETWORK) {
            const char *val = equals + 1;
            int rc = 0;

            if (strcmp(content, "pool") == 0) {
                rc = config_parse_cidr_key(val, "pool", &network->pool_net,
                                           &network->pool_prefix);
                network->pool_set = rc == 0 ? 1U : 0U;
            } else if (strcmp(content, "client_isolation") == 0) {
                int yn = parse_yes_no(val);

                if (yn < 0) {
                    log_message(LOG_ERR, "Invalid value for "
                                "client_isolation: %s", val);
                    rc = -1;
                } else {
                    network->isolation = (int8_t)yn;
                }
            } else if (strcmp(content, "mode") == 0) {
                if (strcasecmp(val, "tun") == 0) {
                    network->tap = 0U;
                } else if (strcasecmp(val, "tap") == 0) {
#ifdef CAMEX_L2
                    network->tap = 1U;
#else
                    log_message(LOG_ERR, "mode = tap needs a camex built "
                                "with CAMEX_L2=1 (camex-<arch>-full)");
                    rc = -1;
#endif
                } else {
                    log_message(LOG_ERR, "Invalid mode: %s (tun or tap)",
                                val);
                    rc = -1;
                }
            } else if (strcmp(content, "mesh") == 0) {
                int yn = parse_yes_no(val);

                if (yn < 0) {
                    log_message(LOG_ERR, "Invalid value for mesh: %s", val);
                    rc = -1;
                } else {
#ifdef CAMEX_MESH
                    network->mesh = (uint8_t)yn;
#else
                    if (yn) {
                        log_message(LOG_ERR, "mesh = yes needs a camex built "
                                    "with CAMEX_MESH=1 (camex-<arch>-full)");
                        rc = -1;
                    }
#endif
                }
            } else if (strcmp(content, "addresses") == 0) {
                if (strcasecmp(val, "pool") == 0) {
                    network->noaddr = 0U;
                } else if (strcasecmp(val, "none") == 0) {
                    network->noaddr = 1U;
                } else {
                    log_message(LOG_ERR, "Invalid addresses: %s (pool or "
                                "none)", val);
                    rc = -1;
                }
            } else if (strcmp(content, "weak_name") == 0) {
                int yn = parse_yes_no(val);

                if (yn < 0) {
                    log_message(LOG_ERR, "Invalid value for weak_name: %s",
                                val);
                    rc = -1;
                } else {
                    network->weak_name = (uint8_t)yn;
                }
            } else if (strcmp(content, "label") == 0) {
                if (strlen(val) > CAMEX_NET_LABEL_MAX ||
                    !camex_name_valid(val) || strchr(val, ' ') != NULL) {
                    log_message(LOG_ERR, "Invalid label=: up to %u bytes "
                                "without spaces or control characters",
                                (unsigned)CAMEX_NET_LABEL_MAX);
                    rc = -1;
                } else {
                    snprintf(network->label, sizeof(network->label), "%s",
                             val);
                }
            } else {
                log_message(LOG_ERR, "Unknown [network] key: %s", content);
                rc = -1;
            }
            if (rc != 0) {
                log_message(LOG_ERR, "%s, line %lu: [network] %s=", path,
                            lineno, content);
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

        /* A display name for the start page (#105): the client's own, so
         * not in [defaults] */
        if (strcmp(content, "name") == 0) {
            if (section != SECTION_CLIENT) {
                log_message(LOG_ERR, "name= belongs in a [client ID] "
                            "block");
                fclose(fp);
                return -1;
            }
            if (!camex_name_valid(equals + 1)) {
                log_message(LOG_ERR, "Invalid name= for client %s: up to %u "
                            "bytes of UTF-8 text without control characters",
                            client->client_id, (unsigned)CAMEX_NAME_MAX);
                fclose(fp);
                return -1;
            }
            snprintf(client->name, sizeof(client->name), "%s", equals + 1);
            continue;
        }

        if (config_db_apply_kv(current, content, equals + 1) != 0) {
            log_message(LOG_ERR, "%s, line %lu: %s=", path, lineno, content);
            fclose(fp);
            return -1;
        }
    }

    fclose(fp);
    if (config_db_check_names() != 0) {
        return -1;
    }
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

int config_db_peer_port(const char *client_id)
{
    const camex_server_profile_t *client = config_db_find_client(client_id);
    int port = server_db.defaults.peer_port;

    if (client != NULL && client->profile.peer_port != 0) {
        port = client->profile.peer_port;
    }
    return port != 0 ? port : CAMEX_PEER_PORT_DEFAULT;
}

int config_db_web_port(const char *client_id, int announced)
{
    const camex_server_profile_t *client = config_db_find_client(client_id);

    if (client != NULL && client->profile.web_port > 0) {
        return client->profile.web_port;
    }
    if (announced > 0 && announced <= 65535) {
        return announced;
    }
    return server_db.defaults.web_port > 0 ? server_db.defaults.web_port : 0;
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

long config_ks_find(const uint8_t *key)
{
    return key != NULL ? ks_find_key(key, NULL, (size_t)-1) : -1L;
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
         * wiped key would pile all of them into one probe chain). Of
         * entries with the same key only the first is indexed by key:
         * every lookup by key wants the first one (the global key is
         * entry 0), and many clients sharing one psk= would otherwise
         * make one long chain */
        if ((!ks->entries[i].locked &&
             ks_find_key(ks->entries[i].psk_key, NULL, i) < 0 &&
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
         * dropping every handshake. Clients of named networks need none */
        if (usable == 0U && !config_networks_enabled()) {
            log_message(LOG_ERR, "No usable PSK: give --psk, psk= in "
                        "[server], or psk= for each client");
            return -1;
        }
    }
    log_message(server_ks.count > 0U ? LOG_INFO : LOG_DEBUG, "Server keystore: %u PSK(s) loaded",
                (unsigned)ks->count);
    return 0;
}

int config_apply_profile(camex_config_t *config, const camex_profile_t *profile)
{
    size_t i;

    if (config == NULL || profile == NULL) {
        return -1;
    }

    /* A TAP network without addresses (addresses = none) has neither */
    if ((profile->local_cidr[0] == '\0' || profile->gateway_ip[0] == '\0') &&
        !(profile->l2 && profile->local_cidr[0] == '\0' &&
          profile->gateway_ip[0] == '\0')) {
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
        config->mtu_given = 1U;   /* the server's: a TAP device keeps it */
    }
    config->l2 = profile->l2;
    config->mesh = profile->mesh;
    memcpy(config->l2_mac, profile->mac, sizeof(config->l2_mac));

    return 0;
}
