/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * config.c — configuration file parser, server DB, profiles
 *
 */

#include "config.h"
#include "crypto.h"
#include "log.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>

camex_server_db_t server_db;
camex_keystore_entry_t server_keystore[CAMEX_MAX_CLIENTS + 1];
size_t server_keystore_count = 0U;

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
    size_t i;

    memset(&server_db, 0, sizeof(server_db));
    config_profile_reset(&server_db.defaults);
    for (i = 0; i < CAMEX_MAX_CLIENTS; ++i) {
        config_profile_reset(&server_db.clients[i].profile);
    }
}

camex_server_profile_t *config_db_find_client(const char *client_id)
{
    size_t i;

    if (client_id == NULL || *client_id == '\0') {
        return NULL;
    }

    for (i = 0; i < CAMEX_MAX_CLIENTS; ++i) {
        if (server_db.clients[i].active &&
            strcasecmp(server_db.clients[i].client_id, client_id) == 0) {
            return &server_db.clients[i];
        }
    }

    return NULL;
}

camex_server_profile_t *config_db_get_client_slot(const char *client_id)
{
    size_t i;
    camex_server_profile_t *slot = NULL;

    if (client_id == NULL || *client_id == '\0') {
        return NULL;
    }

    slot = config_db_find_client(client_id);
    if (slot != NULL) {
        return slot;
    }

    for (i = 0; i < CAMEX_MAX_CLIENTS; ++i) {
        if (!server_db.clients[i].active) {
            slot = &server_db.clients[i];
            memset(slot, 0, sizeof(*slot));
            slot->active = 1U;
            {
                size_t id_len = strlen(client_id);
                if (id_len >= sizeof(slot->client_id)) {
                    log_message(LOG_ERR, "Client ID too long: %s", client_id);
                    return NULL;
                }
                memcpy(slot->client_id, client_id, id_len + 1U);
            }
            config_profile_reset(&slot->profile);
            return slot;
        }
    }

    return NULL;
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

int config_db_load_file(const char *path)
{
    FILE *fp;
    char line[512];
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

    while (fgets(line, sizeof(line), fp) != NULL) {
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
            } else if (strcmp(content, "psk") == 0) {
                rc = copy_option(g->psk, sizeof(g->psk), val, "PSK");
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
    server_db.loaded = 1U;
    (void)section;
    return 0;
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

void config_build_keystore(void)
{
    const camex_crypto_t *ctx;
    size_t i;

    server_keystore_count = 0U;
    memset(server_keystore, 0, sizeof(server_keystore));

    ctx = crypto_get_ctx();
    if (ctx->ready) {
        memcpy(server_keystore[0].psk_key, ctx->psk_key, 32);
        server_keystore_count = 1U;
    }

    for (i = 0U; i < CAMEX_MAX_CLIENTS &&
         server_keystore_count < (size_t)(CAMEX_MAX_CLIENTS + 1); ++i) {
        if (!server_db.clients[i].active) {
            continue;
        }
        if (server_db.clients[i].profile.psk[0] == '\0') {
            continue;
        }
        derive_psk_key(server_db.clients[i].profile.psk,
                       server_keystore[server_keystore_count].psk_key);
        server_keystore_count++;
    }

    for (i = 0U; i < CAMEX_MAX_CLIENTS; ++i) {
        if (server_db.clients[i].profile.psk[0] != '\0') {
            crypto_wipe(server_db.clients[i].profile.psk,
                        sizeof(server_db.clients[i].profile.psk));
        }
    }
    crypto_wipe(server_db.defaults.psk, sizeof(server_db.defaults.psk));

    log_message(LOG_INFO, "Server keystore: %zu PSK(s) loaded",
                server_keystore_count);
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
