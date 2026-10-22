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
#include "slotidx.h"

#include <stdint.h>
#include <stddef.h>

/* Keystore entry */
typedef struct {
    uint8_t psk_key[32];
    char owner[CAMEX_CLIENT_ID_LEN];  /* client ID; empty = global key */
    uint8_t locked;   /* the client's psk= is invalid: the entry keeps the
                       * ID owned but matches no key */
} camex_keystore_entry_t;

/* Per-client profile (from [client X] or defaults) */
typedef struct {
    char local_cidr[32];
    char gateway_ip[16];
    char route_cidrs[CAMEX_MAX_ROUTES][32];
    uint8_t route_count;
    int mtu;
    char psk[128];   /* hex: + 64 digits fits; passphrases too */
} camex_profile_t;

/* Server profile for a named client */
typedef struct {
    char client_id[CAMEX_CLIENT_ID_LEN];
    camex_profile_t profile;
    uint8_t active;
    uint8_t has_ip;     /* ip_be is the address of profile.local_cidr */
    uint32_t ip_be;     /* filled when the file is loaded */
} camex_server_profile_t;

/* Server-level globals from [server] section */
typedef struct {
    char bind_ip[16];
    char local_cidr[32];
    char tun_dev[256];
    char bind_dev[IFNAMSIZ];
    char pid_file[256];
    char psk[128];   /* hex: + 64 digits fits; passphrases too */
    int  port;
    int  mtu;
    uint8_t encrypt;
    uint8_t encrypt_set;
    uint8_t deny_unknown_clients;  /* allow_unknown_clients = no (#15) */
    char server_key[64];           /* base64 private key (server_key=) */
    uint32_t pool_net;             /* auto pool (#75), host byte order */
    uint8_t pool_prefix;           /* 8..30; default 10.200.0.0/24 */
    uint8_t pool_set;              /* auto_pool= was given */
    unsigned int max_clients;      /* max_clients=, 0 = not given */
} camex_server_globals_t;

/* Default auto pool (#15, #75): 10.200.0.0/24 */
#define CAMEX_AUTO_POOL_DEFAULT_NET    0x0AC80000U
#define CAMEX_AUTO_POOL_DEFAULT_PREFIX 24U
#define CAMEX_AUTO_POOL_MIN_PREFIX     8U
#define CAMEX_AUTO_POOL_MAX_PREFIX     30U

/* Full server DB */
/* [client ID] profiles are kept in a growing array, at most
 * CAMEX_MAX_CLIENTS_LIMIT of them (#75) */
typedef struct {
    camex_profile_t defaults;
    camex_server_profile_t *clients;
    size_t client_count;     /* used entries of clients[] */
    size_t client_cap;       /* allocated entries */
    slotidx_t by_id;         /* client ID -> entry */
    slotidx_t by_ip;         /* address of local_cidr -> entry */
    camex_server_globals_t globals;
    uint8_t loaded;
} camex_server_db_t;

/* Keystore: the global key (owner "") and one entry per [client ID] with
 * its own psk=, with indexes by owner and by key (#75) */
typedef struct {
    camex_keystore_entry_t *entries;
    size_t count;
    slotidx_t by_owner;
    slotidx_t by_key;
} camex_keystore_t;

/* Externals (global server DB + keystore) */
extern camex_server_db_t server_db;
extern camex_keystore_t server_ks;
#define server_keystore       (server_ks.entries)
#define server_keystore_count (server_ks.count)

/* Reset profile */
void config_profile_reset(camex_profile_t *profile);

/* Merge src into dst */
void config_profile_merge(camex_profile_t *dst, const camex_profile_t *src);

/* Append a route to a profile */
int config_profile_append_route(camex_profile_t *profile, const char *value);

/* Reset server DB. It does not free: the arrays may belong to a copy the
 * caller keeps (a config reload); config_db_free() releases them. */
void config_server_db_reset(void);

/* Wipe the key texts of db and free its arrays; db is then empty */
void config_db_free(camex_server_db_t *db);

/* Wipe and free a keystore; it is then empty */
void config_keystore_free(camex_keystore_t *ks);

/* Parse "network/prefix" of auto_pool= (#75): prefix 8..30, the network
 * address without host bits. 0, or -1 with a logged reason. */
int config_parse_pool(const char *value, uint32_t *net, uint8_t *prefix);

/* [client ID] profile whose local_cidr address is ip_be, or NULL */
camex_server_profile_t *config_db_find_profile_by_ip(uint32_t ip_be);

/* Keystore queries (#5); keys are 32 bytes */
const char *config_ks_owner(const uint8_t *key);   /* "" = global/none */
int config_ks_has_owner(const char *client_id);
int config_ks_key_owned_by(const uint8_t *key, const char *client_id);
int config_ks_contains(const uint8_t *key);
int config_ks_is_global(const uint8_t *key);
/* Index of the first usable keystore entry with this key, or -1 */
long config_ks_find(const uint8_t *key);

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
/* 0; -1 when no key is usable (no client could connect); KEY_ERR_NOMEM
 * (no memory for Argon2id: the keystore is empty). On an error the caller
 * restores the previous keystore or fails. */
int config_build_keystore(void);

/* Apply a profile to a camex_config_t */
int config_apply_profile(camex_config_t *config, const camex_profile_t *profile);

#endif /* CAMEX_CFG_H */
