/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * camex.h — client/server tunnel configuration and public API
 *
 */

#ifndef CAMEX_H
#define CAMEX_H

/* No server, no start page (#203, #204): a build that leaves out the
 * server (CAMEX_NO_SERVER, as the Android library's) leaves it out too */
#if defined(CAMEX_NO_SERVER) && !defined(CAMEX_NO_WEB)
#define CAMEX_NO_WEB
#endif

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
/* Over UDP 1420: a tunnel packet (MTU + 57 bytes of IPv4, UDP and camex)
 * fits a 1500-byte link and PPPoE's 1492 whole, never two IP fragments.
 * Over TCP 1500: the outer TCP cuts the stream to the path's MSS, so a big
 * inner packet makes no fragments there and a smaller one only costs
 * (#198). The default when no MTU is set: --mtu, mtu= in [server], or the
 * server's CONFIG */
#define CAMEX_DEFAULT_MTU 1420
#define CAMEX_TCP_MTU 1500
/* A TAP device carries the frames of a 1500-byte LAN (camex cuts them into
 * pieces of its own, l2.h): it keeps 1500 unless an MTU is set */
#define CAMEX_L2_MTU 1500
#define CAMEX_DEFAULT_PORT 5800

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
    uint8_t transport;  /* camex_transport_t: 0 UDP, 1 TCP, 2 both (server) */
    camex_mode_t mode;
    char pid_file[256];
    char bind_dev[IFNAMSIZ];
    char tun_dev[256];  /* explicit TUN device path; empty = auto-detect */
    int server_timeout;  /* seconds before declaring connection lost (default 20) */
    int keepalive;       /* keepalive interval in seconds, 0=disabled (default 10) */
    int web_port;        /* client: --web-port, nginx's port on it (#174), 0 = none */
    char server_key_file[256]; /* server: file with its base64 private key */
    char server_pubkey[64];    /* client: the server's base64 public key */
    char network[64];          /* client: --network, the network's name */
    char known_servers[256];   /* client: --known-servers, where the keys of
                                * servers first met are kept; "none" = off */
    uint8_t top;               /* --top: a live view of the server(s) */
    uint8_t pake;              /* client: --network without --psk: CPace
                                * (pake.h) */
    uint8_t pinned;            /* client: --server-pubkey: the server's key
                                * is known, no trust on first use */
    uint8_t mtu_given;         /* --mtu: no default by the transport (#198) */
    uint8_t pake_ok;           /* this connection's CPace succeeded */
    uint8_t send_net_name;     /* the server asked for the network's name
                                * (PAKE_FLAG_NAME) and its key is pinned */
    char bridge_if[IFNAMSIZ];  /* client: --bridge, the LAN interface */
    uint8_t bridge_mode;       /* --bridge-mode: bridge_mode_t */
    uint8_t mesh;              /* client: the server's CONFIG said MESH=1 */
    uint8_t l2;                /* client: the server's CONFIG said MODE=TAP */
    uint8_t l2_mac[6];         /* the MAC it gave for the TAP interface */
    int top_interval;          /* --top -d <seconds> (default 1) */
    char control_path[108];    /* control socket (#82): "" default, "none" */
    uint8_t status_query;      /* --status: ask the server(s), then exit */
    uint8_t status_json;       /* --json */
    uint8_t client_isolation;  /* --client-isolation (#83) */
    uint8_t status_nginx_map;  /* --format nginx-map (#84) */
    uint8_t status_nginx_index;   /* --format nginx-index (#105) */
    char map_domain[254];      /* --domain */
    int map_port;              /* --upstream-port (default 80) */
    int map_http_port;         /* --http-port: nginx's, in the page's links (#141) */
    char map_default[130];     /* --default */
    const char *status_wait;   /* --wait <token> (#130), NULL = none */
    int status_wait_timeout;   /* --wait-timeout, seconds (default 60) */
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

/* Server: notice a changed config file and ask for a reload */
void camex_config_watch(void);

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

/* A pause of about a second (Sleep on Windows, where select() without
 * sockets does not wait) */
void camex_pause_1s(void);

/* Where a client keeps the public keys of the servers it met first
 * without --server-pubkey: "<host>:<port> <base64 key>" lines */
#ifdef _WIN32
#define CAMEX_KNOWN_SERVERS_PATH "camex-known-servers"
#else
#define CAMEX_KNOWN_SERVERS_PATH "/etc/camex/known_servers"
#endif

/* Where a client keeps the ID it took from the MAC until a reboot (#121) */
#define CAMEX_ID_FILE "/tmp/camex-id"

/* The client ID without --name (#121): the one kept in CAMEX_ID_FILE by
 * an earlier run, else the MAC of the default route's interface, waiting
 * for a default route as long as it takes. -1 when stopped first */
int camex_client_id_wait(char *client_id, size_t client_id_size);

#endif
