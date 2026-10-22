/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * camex.h — client/server tunnel configuration and public API
 *
 */

#ifndef CAMEX_H
#define CAMEX_H

#ifndef _WIN32
#ifndef __APPLE__
#include <net/if.h>
#endif
#endif
#include <signal.h>
#include <stdint.h>
#include <time.h>
#include <inttypes.h>

#ifndef IFNAMSIZ
#define IFNAMSIZ 16
#endif

#define CAMEX_MAX_ROUTES 16
/* Live client slots of a server (#75): max_clients= in [server]; without
 * it the size of the auto pool, but at least CAMEX_MAX_CLIENTS_DEFAULT.
 * The limit keeps a slot number within the 16 bits of a session index. */
#define CAMEX_MAX_CLIENTS_DEFAULT 256U
#define CAMEX_MAX_CLIENTS_LIMIT   65534U
#define CAMEX_CLIENT_ID_LEN 32
#define CAMEX_CONFIG_PATH_LEN 256
#define CAMEX_DEFAULT_CONFIG_PATH "/etc/camex/camex.conf"
#define CAMEX_DEFAULT_MTU 1500

typedef enum {
    CAMEX_MODE_CLIENT = 0,
    CAMEX_MODE_SERVER = 1
} camex_mode_t;

typedef struct {
    char bind_ip[16];
    char local_ip[16];
    char local_cidr[32];
    char gateway_ip[16];
    char server_host[64];
    int port;
    char route_cidrs[CAMEX_MAX_ROUTES][32];
    uint8_t route_count;
    int mtu;
    uint8_t encrypt;
    char psk[128];   /* hex: + 64 digits fits; passphrases too */
    char client_id[CAMEX_CLIENT_ID_LEN];
    char config_path[CAMEX_CONFIG_PATH_LEN];
    uint8_t auto_config;
    uint8_t transport;  /* 0=UDP, 1=TCP — matches camex_transport_t */
    camex_mode_t mode;
    char pid_file[256];
    char bind_dev[IFNAMSIZ];
    char tun_dev[256];  /* explicit TUN device path; empty = auto-detect */
    int server_timeout;  /* seconds before declaring connection lost (default 20) */
    int keepalive;       /* keepalive interval in seconds, 0=disabled (default 10) */
    char server_key_file[256]; /* server: file with its base64 private key */
    char server_pubkey[64];    /* client: the server's base64 public key */
    char control_path[108];    /* control socket (#82): "" default, "none" */
    uint8_t status_query;      /* --status: ask the server(s), then exit */
    uint8_t status_json;       /* --json */
    uint8_t client_isolation;  /* --client-isolation (#83) */
    uint8_t status_nginx_map;  /* --format nginx-map (#84) */
    char map_domain[254];      /* --domain */
    int map_port;              /* --upstream-port (default 80) */
    char map_default[130];     /* --default */
} camex_config_t;

/* Global state shared across modules */
extern camex_config_t current_config;
extern volatile sig_atomic_t running;
extern volatile sig_atomic_t reload_config;
extern time_t g_now;
extern uint8_t client_mode;
extern uint8_t server_mode;

int camex_init(camex_config_t *config);
void camex_run(void);
void camex_stop(void);
/* Wipes the keys and closes the sockets and the tunnel; camex_stop() does it
 * too (after removing routes), a failed camex_init() needs it alone */
void camex_cleanup(void);
int camex_add_route(const char *cidr, const char *gateway);

/* Client: apply a changed CONFIG (address, gateway, MTU, routes) to the
 * live tunnel: old routes off, tunnel reopened, new routes on (#49). On
 * failure the old settings are restored; returns 0 or -1. Refused for a
 * TUN fd from CAMEX_TUN_FD, which cannot be reopened. */
int camex_client_reconfigure(const camex_config_t *next);
/* 1 while the tunnel does not match the last CONFIG (a failed route or
 * reopen): the next CONFIG is applied even if it has not changed */
int camex_client_reconfig_pending(void);
/* Client start: install the CONFIG routes on the host (if camex manages
 * them); a route that fails is retried with the next handshake */
void camex_client_install_routes(void);
int camex_del_route(const char *cidr, const char *gateway);

/* Derive client ID from default gateway MAC */
int derive_client_id(char *client_id, size_t client_id_size);

#endif
