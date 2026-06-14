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
#if defined(__APPLE__)
/* macOS requires sys/types.h before net/if.h under strict C99 */
#include <sys/types.h>
#endif
#include <net/if.h>
#endif
#include <signal.h>
#include <stdint.h>
#include <time.h>
#include <inttypes.h>

#ifndef IFNAMSIZ
#define IFNAMSIZ 16
#endif

#define CAMEX_MAX_ROUTES 16
#define CAMEX_MAX_CLIENTS 256
#define CAMEX_CLIENT_ID_LEN 32
#define CAMEX_CONFIG_PATH_LEN 256
#define CAMEX_DEFAULT_CONFIG_PATH "/etc/camex/camex.conf"

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
    char psk[64];
    char client_id[CAMEX_CLIENT_ID_LEN];
    char config_path[CAMEX_CONFIG_PATH_LEN];
    uint8_t auto_config;
    uint8_t transport;  /* 0=UDP, 1=TCP — matches camex_transport_t */
    camex_mode_t mode;
    char pid_file[256];
    char bind_dev[IFNAMSIZ];
    char tun_dev[256];  /* explicit TUN device path; empty = auto-detect */
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
int camex_add_route(const char *cidr, const char *gateway);
int camex_del_route(const char *cidr, const char *gateway);

/* Derive client ID from default gateway MAC */
int derive_client_id(char *client_id, size_t client_id_size);

#endif
