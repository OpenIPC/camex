/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * camex.h — client/server tunnel configuration and public API
 *
 */

#ifndef __CAMEX_H__
#define __CAMEX_H__

#include <net/if.h>
#include <stdint.h>

#define CAMEX_MAX_ROUTES 16
#define CAMEX_CLIENT_ID_LEN 32
#define CAMEX_CONFIG_PATH_LEN 256

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
    camex_mode_t mode;
    char pid_file[256];
    char bind_dev[IFNAMSIZ];
    char tun_dev[256];  /* explicit TUN device path; empty = auto-detect */
} camex_config_t;

int camex_init(camex_config_t *config);
void camex_run(void);
void camex_stop(void);
int camex_add_route(const char *cidr, const char *gateway);
int camex_del_route(const char *cidr, const char *gateway);

#endif
