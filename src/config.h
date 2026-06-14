/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * config.h — configuration file parser, server DB, profiles, keystore
 *
 */

#ifndef CAMEX_CFG_H
#define CAMEX_CFG_H

#include "camex.h"

#include <stdint.h>
#include <stddef.h>

/* Keystore entry */
typedef struct {
    uint8_t psk_key[32];
} camex_keystore_entry_t;

/* Per-client profile (from [client X] or defaults) */
typedef struct {
    char local_cidr[32];
    char gateway_ip[16];
    char route_cidrs[CAMEX_MAX_ROUTES][32];
    uint8_t route_count;
    int mtu;
    char psk[64];
} camex_profile_t;

/* Server profile for a named client */
typedef struct {
    char client_id[CAMEX_CLIENT_ID_LEN];
    camex_profile_t profile;
    uint8_t active;
} camex_server_profile_t;

/* Server-level globals from [server] section */
typedef struct {
    char bind_ip[16];
    char local_cidr[32];
    char tun_dev[256];
    char bind_dev[IFNAMSIZ];
    char pid_file[256];
    char psk[64];
    int  port;
    int  mtu;
    uint8_t encrypt;
    uint8_t encrypt_set;
} camex_server_globals_t;

/* Full server DB */
typedef struct {
    camex_profile_t defaults;
    camex_server_profile_t clients[CAMEX_MAX_CLIENTS];
    camex_server_globals_t globals;
    uint8_t loaded;
} camex_server_db_t;

/* Externals (global server DB + keystore) */
extern camex_server_db_t server_db;
extern camex_keystore_entry_t server_keystore[];
extern size_t server_keystore_count;

/* Reset profile */
void config_profile_reset(camex_profile_t *profile);

/* Merge src into dst */
void config_profile_merge(camex_profile_t *dst, const camex_profile_t *src);

/* Append a route to a profile */
int config_profile_append_route(camex_profile_t *profile, const char *value);

/* Reset server DB */
void config_server_db_reset(void);

/* Find client by ID in server DB */
camex_server_profile_t *config_db_find_client(const char *client_id);

/* Get (or create) client slot in server DB */
camex_server_profile_t *config_db_get_client_slot(const char *client_id);

/* Apply key=value to a profile */
int config_db_apply_kv(camex_profile_t *profile,
                       const char *key, const char *value);

/* Load server config file */
int config_db_load_file(const char *path);

/* Lookup merged profile for a client */
int config_db_lookup_profile(const char *client_id, camex_profile_t *profile);

/* Build the keystore from global PSK and per-client PSKs */
void config_build_keystore(void);

/* Apply a profile to a camex_config_t */
int config_apply_profile(camex_config_t *config, const camex_profile_t *profile);

#endif /* CAMEX_CFG_H */
