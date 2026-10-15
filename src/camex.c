/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * camex.c — client/server UDP/TCP tunnel (main entry point)
 *
 */

#include "camex.h"
#include "client.h"
#include "server.h"
#include "config.h"
#include "crypto.h"
#include "keys.h"
#include "log.h"
#include "main.h"
#include "net.h"
#include "proto.h"
#include "packet.h"
#include "tun.h"
#include "util.h"
#include "version.h"

#ifndef _WIN32
#include <arpa/inet.h>
#endif
#include <errno.h>
#include <getopt.h>
#if defined(__APPLE__)
#include <sys/types.h>
#endif
#ifndef _WIN32
#ifndef __APPLE__
#include <net/route.h>
#endif
#if !defined(__APPLE__)  /* macOS has BSD types, no arp.h needed */
#include <net/if_arp.h>
#endif
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

/* Windows (MinGW) compatibility */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
/* MinGW may not define socklen_t with older headers */
#if !defined(socklen_t)
typedef int socklen_t;
#endif
/* close() does not work on sockets under Windows */
#define close(fd) closesocket(fd)
/* usleep → Sleep (milliseconds) */
#define usleep(us) Sleep((us) / 1000)
/* Windows recvfrom/recv expect char* buffer */
#define RECV_BUF_CAST (char *)
#else
#define RECV_BUF_CAST
#endif
#include <string.h>
#ifndef _WIN32
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/mman.h>
#endif
#ifdef __linux__
#include <sys/prctl.h>
#endif
#ifndef _WIN32
#include <sys/time.h>
#include <sys/syslog.h>
#include <sys/stat.h>
#endif
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if_dl.h>
#include <sys/wait.h>
#endif
#ifdef _WIN32
#include <winsock2.h>
#include <iphlpapi.h>
#endif

/* The global key came from psk= in [server], not from --psk: a reload
 * then replaces it (#16 L3). */
static uint8_t global_psk_from_config = 0U;

/* Global state */
camex_config_t current_config;
volatile sig_atomic_t running = 1;
volatile sig_atomic_t reload_config = 0;
time_t g_now = 0;
uint8_t client_mode = 0;
uint8_t server_mode = 0;

time_t client_reconnect_at = 0;
uint8_t client_link_up = 0;
static uint8_t client_link_confirmed = 0;

#ifdef _WIN32
static void read_default_gateway(char *ifname, size_t size)
{
    ULONG buflen = 0;
    PIP_ADAPTER_INFO pAdapter = NULL;
    PIP_ADAPTER_INFO pAdapterInfo;
    DWORD ret;

    if (ifname == NULL || size == 0U) {
        return;
    }
    ifname[0] = '\0';

    ret = GetAdaptersInfo(NULL, &buflen);
    if (ret != ERROR_BUFFER_OVERFLOW) {
        return;
    }

    pAdapterInfo = (PIP_ADAPTER_INFO)malloc(buflen);
    if (pAdapterInfo == NULL) {
        return;
    }

    if (GetAdaptersInfo(pAdapterInfo, &buflen) == NO_ERROR) {
        pAdapter = pAdapterInfo;
        while (pAdapter != NULL) {
            if (pAdapter->GatewayList.IpAddress.String[0] != '\0' &&
                strcmp(pAdapter->GatewayList.IpAddress.String, "0.0.0.0") != 0) {
                snprintf(ifname, size, "%s", pAdapter->AdapterName);
                break;
            }
            pAdapter = pAdapter->Next;
        }
    }

    free(pAdapterInfo);
}
#elif defined(__APPLE__)
static void read_default_gateway(char *ifname, size_t size)
{
    FILE *fp;
    char line[256];

    if (ifname == NULL || size == 0U) {
        return;
    }
    ifname[0] = '\0';

    fp = popen("/sbin/route -n get default 2>/dev/null", "r");
    if (fp == NULL) {
        return;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char iface[IFNAMSIZ];
        if (sscanf(line, " interface: %15s", iface) == 1) {
            snprintf(ifname, size, "%s", iface);
            break;
        }
    }

    pclose(fp);
}
#else
static void read_default_gateway(char *ifname, size_t size)
{
    FILE *fp;
    char line[256];

    if (ifname == NULL || size == 0U) {
        return;
    }
    ifname[0] = '\0';

    fp = fopen("/proc/net/route", "r");
    if (fp == NULL) {
        return;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char iface[IFNAMSIZ];
        unsigned long destination = 0;
        unsigned long gateway = 0;
        unsigned long flags = 0;

        if (sscanf(line, "%15s %lx %lx %lx",
                   iface, &destination, &gateway, &flags) != 4) {
            continue;
        }

        if (destination == 0UL && (flags & 0x2UL) != 0UL) {
            snprintf(ifname, size, "%s", iface);
            break;
        }
    }

    fclose(fp);
}
#endif

#ifdef _WIN32
static int read_mac(const char *ifname, uint8_t mac[6])
{
    PIP_ADAPTER_INFO pAdapterInfo = NULL;
    PIP_ADAPTER_INFO pAdapter;
    ULONG buflen = sizeof(IP_ADAPTER_INFO);
    DWORD ret;

    if (ifname == NULL || mac == NULL || *ifname == '\0') {
        return -1;
    }

    pAdapterInfo = (PIP_ADAPTER_INFO)malloc(buflen);
    if (pAdapterInfo == NULL) {
        return -1;
    }

    ret = GetAdaptersInfo(pAdapterInfo, &buflen);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        free(pAdapterInfo);
        pAdapterInfo = (PIP_ADAPTER_INFO)malloc(buflen);
        if (pAdapterInfo == NULL) {
            return -1;
        }
        ret = GetAdaptersInfo(pAdapterInfo, &buflen);
    }

    if (ret != NO_ERROR) {
        free(pAdapterInfo);
        return -1;
    }

    pAdapter = pAdapterInfo;
    while (pAdapter != NULL) {
        if (strcmp(pAdapter->AdapterName, ifname) == 0 &&
            pAdapter->AddressLength >= 6) {
            memcpy(mac, pAdapter->Address, 6);
            free(pAdapterInfo);
            return 0;
        }
        pAdapter = pAdapter->Next;
    }

    free(pAdapterInfo);
    return -1;
}
#elif defined(__APPLE__)
static int read_mac(const char *ifname, uint8_t mac[6])
{
    struct ifaddrs *ifap, *ifa;
    int ret = -1;

    if (ifname == NULL || mac == NULL || *ifname == '\0') {
        return -1;
    }

    if (getifaddrs(&ifap) != 0) {
        return -1;
    }

    for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL ||
            ifa->ifa_addr->sa_family != AF_LINK) {
            continue;
        }
        if (strcmp(ifa->ifa_name, ifname) != 0) {
            continue;
        }
        struct sockaddr_dl *sdl = (struct sockaddr_dl *)ifa->ifa_addr;
        if (sdl->sdl_alen >= 6) {
            memcpy(mac, LLADDR(sdl), 6);
            ret = 0;
            break;
        }
    }

    freeifaddrs(ifap);
    return ret;
}
#else
static int read_mac(const char *ifname, uint8_t mac[6])
{
    struct ifreq ifr;
    int sock;

    if (ifname == NULL || mac == NULL || *ifname == '\0') {
        return -1;
    }

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        print_errno_message(LOG_ERR, "socket(AF_INET, SOCK_DGRAM)");
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    if (ioctl(sock, SIOCGIFHWADDR, &ifr) < 0) {
        print_errno_message(LOG_ERR, "ioctl(SIOCGIFHWADDR)");
        close(sock);
        return -1;
    }

    close(sock);

    if (ifr.ifr_hwaddr.sa_family != ARPHRD_ETHER) {
        return -1;
    }

    memcpy(mac, ifr.ifr_hwaddr.sa_data, 6U);
    if (memcmp(mac, "\0\0\0\0\0\0", 6U) == 0) {
        return -1;
    }

    return 0;
}
#endif

int derive_client_id(char *client_id, size_t client_id_size)
{
    char ifname[IFNAMSIZ];
    uint8_t mac[6];
    size_t i;

    if (client_id == NULL || client_id_size == 0U) {
        return -1;
    }

    read_default_gateway(ifname, sizeof(ifname));
    if (ifname[0] == '\0') {
        return -1;
    }

    if (read_mac(ifname, mac) != 0) {
        return -1;
    }

    if (client_id_size <= 12U) {
        return -1;
    }

    for (i = 0; i < 6U; ++i) {
        snprintf(client_id + (i * 2U), client_id_size - (i * 2U),
                 "%02X", mac[i]);
    }
    client_id[12] = '\0';
    return 0;
}

static const char *mode_to_string(camex_mode_t mode)
{
    switch (mode) {
    case CAMEX_MODE_CLIENT:
        return "client";
    case CAMEX_MODE_SERVER:
        return "server";
    default:
        return "unknown";
    }
}

/* Protocol v2 keys (#17): the server's static key pair (--server-key-file
 * wins over server_key= in [server]); on a client, the server's public
 * key. */
static int camex_load_v2_keys(void)
{
    int rc = 0;

    if (server_mode) {
        char text[128];

        text[0] = '\0';
        if (current_config.server_key_file[0] != '\0') {
            if (read_key_file(current_config.server_key_file, text,
                              sizeof(text)) != 0) {
                log_message(LOG_ERR, "Cannot read the server key file %s",
                            current_config.server_key_file);
                return -1;
            }
        } else if (server_db.globals.server_key[0] != '\0') {
            snprintf(text, sizeof(text), "%s", server_db.globals.server_key);
        }
        if (text[0] != '\0') {
            rc = crypto_set_server_key(text);
        }
        crypto_wipe(text, sizeof(text));
        crypto_wipe(server_db.globals.server_key,
                    sizeof(server_db.globals.server_key));
    } else if (current_config.server_pubkey[0] != '\0') {
        rc = crypto_set_server_pub(current_config.server_pubkey);
    }
    return rc;
}

int camex_init(camex_config_t *config)
{
    char local_ip[16];
    char local_netmask[16];
    char endpoint[64];

    if (config == NULL) {
        log_message(LOG_ERR, "Configuration is missing");
        return -1;
    }

    memcpy(&current_config, config, sizeof(current_config));
    client_mode = (config->mode == CAMEX_MODE_CLIENT) ? 1U : 0U;
    server_mode = (config->mode == CAMEX_MODE_SERVER) ? 1U : 0U;
    client_state_init(&client_state);
    memset(server_clients, 0, (size_t)CAMEX_MAX_CLIENTS * sizeof(server_client_t));

    if (client_mode && current_config.keepalive == 0) {
        log_message(LOG_WARNING,
                    "Keepalive is off: a lost server is not detected "
                    "(the server timeout needs keepalives)");
    }

    if (server_mode) {
        if (config_db_load_file(current_config.config_path) != 0) {
            return -1;
        }

        {
            const camex_server_globals_t *g = &server_db.globals;

            if (current_config.port == 0 && g->port > 0) {
                current_config.port = g->port;
            }
            if (current_config.bind_ip[0] == '\0' && g->bind_ip[0] != '\0') {
                snprintf(current_config.bind_ip, sizeof(current_config.bind_ip),
                         "%s", g->bind_ip);
            }
            if (current_config.local_cidr[0] == '\0' &&
                g->local_cidr[0] != '\0') {
                snprintf(current_config.local_cidr,
                         sizeof(current_config.local_cidr), "%s",
                         g->local_cidr);
            }
            if (current_config.tun_dev[0] == '\0' && g->tun_dev[0] != '\0') {
                snprintf(current_config.tun_dev, sizeof(current_config.tun_dev),
                         "%s", g->tun_dev);
            }
            if (current_config.bind_dev[0] == '\0' &&
                g->bind_dev[0] != '\0') {
                snprintf(current_config.bind_dev,
                         sizeof(current_config.bind_dev), "%s", g->bind_dev);
            }
            if (current_config.pid_file[0] == '\0' &&
                g->pid_file[0] != '\0') {
                snprintf(current_config.pid_file,
                         sizeof(current_config.pid_file), "%s", g->pid_file);
            }
            if (current_config.psk[0] == '\0' && g->psk[0] != '\0') {
                snprintf(current_config.psk, sizeof(current_config.psk),
                         "%s", g->psk);
                global_psk_from_config = 1U;
            }
            if (current_config.mtu == CAMEX_DEFAULT_MTU && g->mtu > 0) {
                current_config.mtu = g->mtu;
            }
        }

        if (current_config.port <= 0) {
            log_message(LOG_ERR,
                "Server port must be specified (use -p or port= in [server])");
            return -1;
        }
    }

    /* Protocol v2 is always encrypted (#17) */
    {
        int keys_ok = crypto_init(current_config.psk) == 0 &&
                      camex_load_v2_keys() == 0;

        if (keys_ok && server_mode && !crypto_get_ctx()->has_server_key) {
            log_message(LOG_ERR, "The server needs its private key: "
                        "--server-key-file or server_key= in [server] "
                        "(create one with --genkey)");
            keys_ok = 0;
        }
        if (keys_ok && client_mode && (!crypto_get_ctx()->has_server_pub ||
                                       !crypto_get_ctx()->has_global)) {
            log_message(LOG_ERR, "The client needs --psk and the server's "
                        "public key (--server-pubkey)");
            keys_ok = 0;
        }
        /* The key texts are not needed any more; on failure the derived
         * keys go too (on Android the process outlives this start) */
        crypto_wipe(current_config.psk, sizeof(current_config.psk));
        crypto_wipe(server_db.globals.psk, sizeof(server_db.globals.psk));
        crypto_wipe(server_db.globals.server_key,
                    sizeof(server_db.globals.server_key));
        if (!keys_ok) {
            crypto_wipe_ctx();
            key_cache_clear();
            crypto_wipe(&server_db, sizeof(server_db));  /* client psk= */
            return -1;
        }
    }

    if (client_mode) {
        if (!current_config.auto_config &&
            validate_and_prepare_client(&current_config) != 0) {
            return -1;
        }

        {
            unsigned int dns_attempt = 0;
            while (running) {
                if (client_socket_create(current_config.server_host,
                                         current_config.port,
                                         current_config.bind_dev) == 0) {
                    break;
                }
                dns_attempt++;
                if (dns_attempt == 1 || dns_attempt % 12 == 0) {
                    log_message(LOG_WARNING,
                        "Cannot reach server %s:%d, retrying every 5s...",
                        current_config.server_host, current_config.port);
                }
                {
                    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
                    fd_set dummy;
                    FD_ZERO(&dummy);
                    select(0, NULL, NULL, NULL, &tv);
                }
            }
            if (!running) {
                return -1;
            }
        }

        client_state.last_recv = time(NULL);
        client_state.last_keepalive = time(NULL);

        if (current_config.auto_config) {
            g_now = time(NULL);
            if (client_start_handshake() != 0) {
                log_message(LOG_WARNING, "Initial handshake failed");
            }
            if (client_wait_for_config() != 0) {
                net_close();
                return -1;
            }
        }

        if (parse_local_cidr(current_config.local_cidr, local_ip,
                             sizeof(local_ip), local_netmask,
                             sizeof(local_netmask)) != 0) {
            net_close();
            return -1;
        }

        {
            camex_packet_open_t open_params;

            memset(&open_params, 0, sizeof(open_params));
            open_params.local_ip = local_ip;
            open_params.netmask = local_netmask;
            open_params.mtu = current_config.mtu;
            open_params.routes =
                (const char (*)[32])current_config.route_cidrs;
            open_params.route_count = current_config.route_count;
            open_params.device = current_config.tun_dev;
            open_params.use_provider = 1U;
            if (packet_open(&open_params) != 0) {
                net_close();
                return -1;
            }
        }

        g_now = time(NULL);
        if (!current_config.auto_config && client_start_handshake() != 0) {
            log_message(LOG_WARNING, "Initial handshake failed");
        }

        client_link_up = 1U;
        log_message(LOG_NOTICE,
                    "Tunnel started, awaiting response from server %s:%d",
                    current_config.server_host, current_config.port);
        log_message(LOG_INFO, "Tunnel initialized");
        log_message(LOG_INFO, "  Mode: %s", mode_to_string(current_config.mode));
        log_message(LOG_INFO, "  Server: %s:%d",
                    current_config.server_host, current_config.port);
        log_message(LOG_INFO, "  Local CIDR: %s", current_config.local_cidr);
        log_message(LOG_INFO, "  Tunnel device: %s", tun_name);
        log_message(LOG_INFO, "  MTU: %d", current_config.mtu);
        log_message(LOG_INFO, "  Protocol: v%u (%s)", (unsigned)CAMEX_PROTO_VER,
                    "Noise_NKpsk0_25519_ChaChaPoly_BLAKE2b");
        log_message(LOG_INFO, "  Transport: %s",
                    current_config.transport == CAMEX_TRANSPORT_TCP
                        ? "TCP" : "UDP");
        return 0;
    }

    if (config_build_keystore() != 0) {
        return -1;
    }

    if (server_socket_create(current_config.bind_ip, current_config.port,
                             current_config.bind_dev) != 0) {
        return -1;
    }

    snprintf(endpoint, sizeof(endpoint), "%s:%d",
             current_config.bind_ip[0] != '\0'
                 ? current_config.bind_ip : "0.0.0.0",
             current_config.port);

    if (current_config.local_cidr[0] != '\0') {
        if (parse_local_cidr(current_config.local_cidr, local_ip,
                             sizeof(local_ip), local_netmask,
                             sizeof(local_netmask)) != 0) {
            net_close();
            return -1;
        }

        {
            camex_packet_open_t open_params;

            memset(&open_params, 0, sizeof(open_params));
            open_params.local_ip = local_ip;
            open_params.netmask = local_netmask;
            open_params.mtu = current_config.mtu;
            open_params.device = current_config.tun_dev;
            if (packet_open(&open_params) != 0) {
                net_close();
                return -1;
            }
        }
    }

    log_message(LOG_INFO, "Tunnel initialized");
    log_message(LOG_INFO, "  Mode: %s", mode_to_string(current_config.mode));
    log_message(LOG_INFO, "  Listen endpoint: %s", endpoint);
    if (current_config.local_cidr[0] != '\0') {
        log_message(LOG_INFO, "  Local CIDR: %s", current_config.local_cidr);
        log_message(LOG_INFO, "  Tunnel device: %s", tun_name);
    }
    log_message(LOG_INFO, "  MTU: %d", current_config.mtu);
    log_message(LOG_INFO, "  Protocol: v%u (%s)", (unsigned)CAMEX_PROTO_VER,
                "Noise_NKpsk0_25519_ChaChaPoly_BLAKE2b");
    log_message(LOG_INFO, "  Transport: %s",
                current_config.transport == CAMEX_TRANSPORT_TCP
                    ? "TCP" : "UDP");
    g_now = time(NULL);
    return 0;
}

static void drain_udp_packets(void)
{
    uint8_t buffer[TUN_PACKET_MAX + V2_OVERHEAD];
    struct sockaddr_in from;
    ssize_t len;

    if (server_mode) {
        while (1) {
            socklen_t fromlen = sizeof(from);

            len = recvfrom(net_fd, RECV_BUF_CAST buffer, sizeof(buffer), 0,
                           (struct sockaddr *)&from, &fromlen);
            if (len > 0) {
                if (server_handle_packet(buffer, (size_t)len, &from, -1) != 0) {
                    char peer[64];
                    net_sockaddr_to_string(&from, peer, sizeof(peer));
                    log_peer(LOG_WARNING,
                             "Dropped packet from %s", peer);
                }
                continue;
            }

            if (len < 0 && net_sock_err_is_again()) {
                break;
            }

            break;
        }
        return;
    }

    /* Client mode TCP */
    if (current_config.transport == CAMEX_TRANSPORT_TCP) {
        size_t frame_len;
        int rc = net_tcp_recv_frame(net_fd, buffer, sizeof(buffer),
                                    &frame_len, &client_recv_state);
        if (rc == 0) {
            if (!client_link_confirmed) {
                client_link_confirmed = 1U;
                log_message(LOG_NOTICE,
                    "Connection to server %s:%d established "
                    "(first response received)",
                    current_config.server_host, current_config.port);
            }
            if (client_handle_net_packet(buffer, frame_len) != 0) {
                log_peer(LOG_WARNING,
                         "Dropped packet from server");
            }
        } else if (rc < 0) {
            log_message(LOG_WARNING,
                        "TCP read error from server — will reconnect");
            net_close();
        }
        /* rc == 1 => EAGAIN, just return */
        return;
    }

    /* UDP client: drain all available packets */
    while (1) {
        len = recv(net_fd, RECV_BUF_CAST buffer, sizeof(buffer), 0);
        if (len > 0) {
            if (!client_link_confirmed) {
                client_link_confirmed = 1U;
                log_message(LOG_NOTICE,
                    "Connection to server %s:%d established "
                    "(first response received)",
                    current_config.server_host, current_config.port);
            }
            if (client_handle_net_packet(buffer, (size_t)len) != 0) {
                log_peer(LOG_WARNING,
                         "Dropped packet from server");
            }
            continue;
        }

        if (len < 0 && net_sock_err_is_again()) {
            break;
        }

        break;
    }
}

static void drain_tcp_server_accept(void)
{
    struct sockaddr_in peer;
    size_t i;
    char peer_str[64];

    /* Accept new connections (nonblocking loop) */
    while (1) {
        int client_fd;
        socklen_t addrlen;

        memset(&peer, 0, sizeof(peer));
        addrlen = sizeof(peer);
        client_fd = accept(listen_fd, (struct sockaddr *)&peer, &addrlen);
        if (client_fd < 0) {
            if (net_sock_err_is_again()) {
                break;  /* no pending connections */
            }
            /* Retry on transient errors (EINTR, ECONNABORTED, EPROTO) */
            continue;
        }

#ifndef _WIN32
        /* select() can only watch descriptors below FD_SETSIZE (#16 L4) */
        if (client_fd >= FD_SETSIZE) {
            net_tcp_close(client_fd);
            log_peer(LOG_WARNING,
                     "Refused TCP connection: descriptor %d is beyond "
                     "FD_SETSIZE", client_fd);
            continue;
        }
#endif
        if (set_fd_nonblocking(client_fd) != 0) {
            net_tcp_close(client_fd);
            continue;
        }

        /* Until it registers, a connection holds a pre-auth slot (#9):
         * at most CAMEX_TCP_PREAUTH_PER_IP per source address (more are
         * refused) and CAMEX_TCP_PREAUTH_MAX in total (the oldest one past
         * CAMEX_TCP_PREAUTH_GRACE is closed to make room: a real client
         * registers within a round trip, a flood cannot keep it out),
         * each closed CAMEX_TCP_PREAUTH_TIMEOUT after the accept. */
        {
            size_t pending = 0U;
            size_t pending_ip = 0U;
            server_client_t *oldest = NULL;

            for (i = 0; i < CAMEX_MAX_CLIENTS; ++i) {
                server_client_t *c = &server_clients[i];

                if (!c->active || c->tcp_fd < 0 || c->registered) {
                    continue;
                }
                ++pending;
                if (c->addr.sin_addr.s_addr == peer.sin_addr.s_addr) {
                    ++pending_ip;
                }
                if (difftime(g_now, c->connected_at) >=
                        (double)CAMEX_TCP_PREAUTH_GRACE &&
                    (oldest == NULL || c->connected_at < oldest->connected_at)) {
                    oldest = c;
                }
            }
            if (pending_ip >= CAMEX_TCP_PREAUTH_PER_IP) {
                net_tcp_close(client_fd);
                net_sockaddr_to_string(&peer, peer_str, sizeof(peer_str));
                log_peer(LOG_WARNING,
                         "Refused TCP connection from %s: too many "
                         "unregistered connections from this address",
                         peer_str);
                continue;
            }
            if (pending >= CAMEX_TCP_PREAUTH_MAX && oldest == NULL) {
                /* All pre-auth connections are younger than the grace
                 * period: refuse; the client retries */
                net_tcp_close(client_fd);
                net_sockaddr_to_string(&peer, peer_str, sizeof(peer_str));
                log_peer(LOG_WARNING,
                         "Refused TCP connection from %s: too many "
                         "unregistered connections", peer_str);
                continue;
            }
            if (pending >= CAMEX_TCP_PREAUTH_MAX) {
                net_sockaddr_to_string(&oldest->addr, peer_str,
                                       sizeof(peer_str));
                log_peer(LOG_WARNING,
                         "Closed unregistered TCP connection from %s: "
                         "too many unregistered connections", peer_str);
                net_tcp_close(oldest->tcp_fd);
                memset(oldest, 0, sizeof(*oldest));
            }
        }

        for (i = 0; i < CAMEX_MAX_CLIENTS; ++i) {
            if (!server_clients[i].active) {
                memset(&server_clients[i], 0, sizeof(server_clients[i]));
                server_clients[i].tcp_fd = client_fd;
                server_clients[i].addr = peer;
                server_clients[i].last_seen = g_now;
                server_clients[i].connected_at = g_now;
                server_clients[i].active = 1U;
                net_sockaddr_to_string(&peer, peer_str, sizeof(peer_str));
                log_message(LOG_INFO, "TCP client connected: %s (fd=%d)",
                            peer_str, client_fd);
                break;
            }
        }
        if (i >= CAMEX_MAX_CLIENTS) {
            net_tcp_close(client_fd);
            net_sockaddr_to_string(&peer, peer_str, sizeof(peer_str));
            log_message(LOG_WARNING,
                        "No free slots for TCP client: %s", peer_str);
        }
    }
}

static void drain_tcp_client_read(int fd)
{
    uint8_t buffer[TUN_PACKET_MAX + V2_OVERHEAD];
    size_t frame_len;
    size_t i;
    char peer_str[64];
    int rc;
    tcp_recv_state_t *rstate = NULL;

    /* Find this fd's own persistent partial-read tracker up front — each
     * TCP client has its own (see server_client_t.recv_state in server.h). */
    for (i = 0; i < CAMEX_MAX_CLIENTS; ++i) {
        if (server_clients[i].active && server_clients[i].tcp_fd == fd) {
            rstate = &server_clients[i].recv_state;
            break;
        }
    }
    if (rstate == NULL) {
        net_tcp_close(fd);
        return;
    }

    rc = net_tcp_recv_frame(fd, buffer, sizeof(buffer), &frame_len, rstate);
    if (rc == 1) {
        return;  /* EAGAIN — partial data; will re-select */
    }
    if (rc < 0) {
        /* Connection closed or error — find and remove this client */
        for (i = 0; i < CAMEX_MAX_CLIENTS; ++i) {
            if (server_clients[i].active &&
                server_clients[i].tcp_fd == fd) {
                net_sockaddr_to_string(&server_clients[i].addr,
                                       peer_str, sizeof(peer_str));
                log_message(LOG_INFO,
                            "TCP client disconnected: %s (fd=%d)",
                            peer_str, fd);
                server_drop_entry(&server_clients[i]);
                return;
            }
        }
        net_tcp_close(fd);
        return;
    }

    /* Find the client entry for this fd */
    for (i = 0; i < CAMEX_MAX_CLIENTS; ++i) {
        if (server_clients[i].active &&
            server_clients[i].tcp_fd == fd) {
            /* last_seen is refreshed by server_handle_packet() only for
             * packets that were accepted: a frame that fails to decrypt
             * must not keep the connection alive (#9). A positive result
             * is a forwarded frame still queued for a slow TCP peer.
             * from is a copy: a handshake may move this connection to the
             * client's existing entry and wipe this slot. */
            struct sockaddr_in from = server_clients[i].addr;

            if (server_handle_packet(buffer, frame_len, &from, fd) < 0) {
                net_sockaddr_to_string(&from, peer_str, sizeof(peer_str));
                log_peer(LOG_WARNING,
                         "Dropped TCP packet from %s",
                         peer_str);
            }
            return;
        }
    }
}

/* Max TUN packets handled per event-loop pass. */
#define TUN_DRAIN_BUDGET 64U

static void handle_tun_packet(void)
{
    uint8_t buffer[TUN_PACKET_MAX];
    ssize_t len;
    uint32_t src_ip_be = 0;
    uint32_t dst_ip_be = 0;
    unsigned int budget = TUN_DRAIN_BUDGET;

    /* Bounded: under a continuous stream the loop returns to select()
     * after TUN_DRAIN_BUDGET packets, so socket reads, pending-send
     * flushes and client expiry still run.  The TUN fd stays readable,
     * so the next pass picks up where this one stopped. */
    while (budget-- > 0U) {
        len = packet_read(buffer, sizeof(buffer));
        if (len > 0) {
            int rc;
            if (server_mode) {
                if (ipv4_parse_endpoints(buffer, (size_t)len,
                                         &src_ip_be, &dst_ip_be) != 0) {
                    rc = 0;
                } else {
                    rc = server_forward_packet(buffer, (size_t)len,
                                               src_ip_be, dst_ip_be);
                }
            } else {
                rc = client_handle_tun_packet(buffer, (size_t)len);
            }
            if (rc == 1) {
                /* rc==1: the destination TCP connection is still flushing.
                 * Either an earlier frame was pending and this packet was
                 * dropped, or this frame was queued and is partly sent
                 * (net_tcp_send_frame(), one pending buffer per fd).  A
                 * client has a single connection, so stop draining TUN and
                 * let the flush finish; the kernel's TUN queue holds the
                 * rest until the next event-loop pass.  A server keeps
                 * draining: other clients' sockets are not blocked, and one
                 * stalled client must not hold up their traffic. */
                if (!server_mode) {
                    break;
                }
                continue;
            }
            if (rc != 0) {
                log_message(LOG_WARNING, "Dropped packet from tunnel device");
            }
            continue;
        }

        if (len < 0 && net_sock_err_is_again()) {
            break;
        }

        if (len < 0) {
            print_errno_message(LOG_WARNING, "read(tun)");
        }
        break;
    }
}

void camex_run(void)
{
    fd_set readset;
    fd_set writeset;
    struct timeval tv;
    int maxfd;
    int ready;

    if (server_mode) {
        log_message(LOG_INFO, "Server started. Press Ctrl+C to stop.");
        while (running) {
            size_t si;

            g_now = time(NULL);
            FD_ZERO(&readset);
            FD_ZERO(&writeset);

            if (current_config.transport == CAMEX_TRANSPORT_UDP) {
                if (net_fd >= 0) {
                    FD_SET(net_fd, &readset);
                }
                maxfd = net_fd;
            } else {
                /* TCP: monitor listen_fd + all active client fds */
                if (listen_fd >= 0) {
                    FD_SET(listen_fd, &readset);
                }
                maxfd = listen_fd;
                for (si = 0; si < CAMEX_MAX_CLIENTS; ++si) {
                    if (server_clients[si].active &&
                        server_clients[si].tcp_fd >= 0) {
                        FD_SET(server_clients[si].tcp_fd, &readset);
                        if (server_clients[si].tcp_fd > maxfd) {
                            maxfd = server_clients[si].tcp_fd;
                        }
                    }
                }
            }

            if (packet_fd() >= 0) {
                FD_SET(packet_fd(), &readset);
                if ((int)packet_fd() > maxfd) {
                    maxfd = (int)packet_fd();
                }
            }

            /* Monitor TCP socket writability for pending sends */
            maxfd = net_tcp_pending_fdset(&writeset, maxfd);

            tv.tv_sec = 1;
            tv.tv_usec = 0;

            ready = select(maxfd + 1, &readset, &writeset, NULL, &tv);
            if (ready > 0) {
                /* Retry pending TCP sends when sockets become writable */
                net_tcp_flush_writable(&writeset);
                if (current_config.transport == CAMEX_TRANSPORT_UDP) {
                    if (net_fd >= 0 && FD_ISSET(net_fd, &readset)) {
                        drain_udp_packets();
                    }
                } else {
                    /* TCP: accept new connections */
                    if (listen_fd >= 0 && FD_ISSET(listen_fd, &readset)) {
                        drain_tcp_server_accept();
                    }
                    /* Read from active TCP clients */
                    for (si = 0; si < CAMEX_MAX_CLIENTS; ++si) {
                        if (server_clients[si].active &&
                            server_clients[si].tcp_fd >= 0 &&
                            FD_ISSET(server_clients[si].tcp_fd, &readset)) {
                            drain_tcp_client_read(
                                server_clients[si].tcp_fd);
                        }
                    }
                }
                if (packet_fd() >= 0 && FD_ISSET(packet_fd(), &readset)) {
                    handle_tun_packet();
                }
            } else if (ready < 0 && errno != EINTR) {
                print_errno_message(LOG_ERR, "select");
            }

            server_expire_clients();
            log_limit_tick();
            if (reload_config) {
                camex_server_db_t saved_db = server_db;
                static camex_keystore_entry_t saved_keystore[CAMEX_MAX_CLIENTS + 1];
                size_t saved_keystore_count = server_keystore_count;
                int restore;
                camex_crypto_t saved_ctx;

                reload_config = 0;
                log_message(LOG_INFO, "Reloading config: %s",
                            current_config.config_path);
                key_cache_mark();
                crypto_save_ctx(&saved_ctx);
                memcpy(saved_keystore, server_keystore,
                       sizeof(saved_keystore));
                restore = config_db_load_file(current_config.config_path) != 0;

                if (!restore) {
                    /* The global key follows psk= in [server] when it came
                     * from there (--psk on the command line wins) */
                    if (global_psk_from_config) {
                        if (server_db.globals.psk[0] != '\0') {
                            /* A bad new key must not leave the old one
                             * working */
                            int rc = crypto_init(server_db.globals.psk);

                            if (rc == KEY_ERR_NOMEM) {
                                restore = 1;   /* the key may be fine */
                            } else if (rc != 0) {
                                log_message(LOG_ERR, "Invalid global psk= "
                                            "after the reload: the global "
                                            "key is disabled until it is "
                                            "fixed");
                                crypto_drop_global();
                            }
                            crypto_wipe(server_db.globals.psk,
                                        sizeof(server_db.globals.psk));
                        } else {
                            log_message(LOG_NOTICE,
                                        "Global key removed by the reload");
                            crypto_drop_global();
                        }
                    }
                    /* The server key is read at startup only */
                    if (server_db.globals.server_key[0] != '\0') {
                        log_message(LOG_NOTICE, "server_key= is read at "
                                    "startup only: restart to change it");
                        crypto_wipe(server_db.globals.server_key,
                                    sizeof(server_db.globals.server_key));
                    }
                    /* Out of memory for Argon2id: nothing is wrong with
                     * the keys, so keep the previous ones */
                    if (!restore && config_build_keystore() != 0) {
                        restore = 1;
                    }
                    if (!restore) {
                        server_revalidate_keys();
                        server_revalidate_unknown_clients();
                    }
                }
                if (restore) {
                    crypto_wipe(&server_db, sizeof(server_db));
                    server_db = saved_db;
                    memcpy(server_keystore, saved_keystore,
                           sizeof(saved_keystore));
                    server_keystore_count = saved_keystore_count;
                    crypto_restore_ctx(&saved_ctx);
                    log_message(LOG_WARNING,
                                "Keeping previous server configuration");
                }
                /* Cached keys of the rejected configuration go as well
                 * (those of the kept one come back from Argon2id if a
                 * later reload needs them) */
                key_cache_sweep();
                crypto_wipe(&saved_ctx, sizeof(saved_ctx));
                /* The copies hold passphrases and keys of the previous
                 * config */
                crypto_wipe(&saved_db, sizeof(saved_db));
                crypto_wipe(saved_keystore, sizeof(saved_keystore));
            }
        }

        log_message(LOG_INFO, "Server stopped.");
        return;
    }

    log_message(LOG_INFO, "Client started. Press Ctrl+C to stop.");
    while (running) {
        g_now = time(NULL);
        log_limit_tick();

        if (net_fd < 0) {
            usleep(1000000);
            client_tick();
            continue;
        }

        FD_ZERO(&readset);
        FD_ZERO(&writeset);
        FD_SET(net_fd, &readset);
        if (packet_fd() >= 0) {
            FD_SET(packet_fd(), &readset);
        }

        tv.tv_sec = 1;
        tv.tv_usec = 0;

        maxfd = net_fd;
        if ((int)packet_fd() > maxfd) {
            maxfd = (int)packet_fd();
        }
        /* Monitor TCP socket writability for pending sends */
        maxfd = net_tcp_pending_fdset(&writeset, maxfd);

        ready = select(maxfd + 1, &readset, &writeset, NULL, &tv);
        if (ready > 0) {
            /* Retry pending TCP sends when sockets become writable */
            net_tcp_flush_writable(&writeset);
            if (FD_ISSET(net_fd, &readset)) {
                drain_udp_packets();
            }
            if (packet_fd() >= 0 && FD_ISSET(packet_fd(), &readset)) {
                handle_tun_packet();
            }
        } else if (ready < 0 && errno != EINTR) {
            print_errno_message(LOG_ERR, "select");
        }

        client_tick();
    }

    log_message(LOG_INFO, "Client stopped.");
}

void camex_stop(void)
{
    size_t i;

    running = 0;

    if (client_mode && current_config.gateway_ip[0] != '\0' &&
        packet_host_routes()) {
        for (i = 0; i < current_config.route_count; ++i) {
            if (camex_del_route(current_config.route_cidrs[i],
                                current_config.gateway_ip) != 0) {
                log_message(LOG_WARNING, "Failed to delete route for %s",
                            current_config.route_cidrs[i]);
            }
        }
    }

    /* Wipe all key material from memory */
    crypto_wipe_ctx();
    {
        /* Wipe per-client keys and the passphrase key cache */
        key_cache_clear();
        extern camex_keystore_entry_t server_keystore[];
        extern size_t server_keystore_count;
        for (i = 0; i < server_keystore_count; ++i) {
            crypto_wipe(server_keystore[i].psk_key,
                        sizeof(server_keystore[i].psk_key));
        }
        server_keystore_count = 0;
    }
    {
        extern server_client_t server_clients[];
        for (i = 0; i < (size_t)CAMEX_MAX_CLIENTS; ++i) {
            crypto_wipe(&server_clients[i], sizeof(server_clients[i]));
        }
    }
    client_wipe();
    crypto_wipe_ctx();
    crypto_wipe(current_config.psk, sizeof(current_config.psk));

    packet_close();
    net_close();
}

#ifdef _WIN32
static int run_ip_route(const char *verb, const char *cidr, const char *gateway)
{
    (void)verb;
    (void)cidr;
    (void)gateway;
    log_message(LOG_WARNING, "Route management is not supported on Windows");
    return 0;  /* non-fatal: continue without adding routes */
}
#elif defined(__APPLE__)
static int run_ip_route(const char *verb, const char *cidr, const char *gateway)
{
    /*
     * macOS: use /sbin/route instead of ioctl(SIOCADDRT), which Darwin
     * does not support. Run it directly with an argument vector, never
     * through a shell: cidr and gateway may come from the server's CONFIG
     * message (#12).
     */
    char *argv[7];
    pid_t pid;
    int status = 0;

    if (verb == NULL || cidr == NULL || gateway == NULL) {
        return -1;
    }

    argv[0] = "/sbin/route";
    argv[1] = "-n";
    argv[2] = (strcmp(verb, "add") == 0 || strcmp(verb, "replace") == 0)
                  ? "add" : "delete";
    argv[3] = "-inet";
    argv[4] = (char *)cidr;
    argv[5] = (char *)gateway;
    argv[6] = NULL;

    pid = fork();
    if (pid < 0) {
        print_errno_message(LOG_ERR, "fork(/sbin/route)");
        return -1;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);

        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}
#else
static int run_ip_route(const char *verb, const char *cidr, const char *gateway)
{
    struct rtentry rt;
    struct sockaddr_in *dst, *gw, *msk;
    char cidr_buf[32];
    char *slash;
    char *end = NULL;
    long prefix;
    uint32_t netmask_val;
    struct in_addr dst_addr;
    struct in_addr gw_addr;
    int sock;
    int add;
    int ret;

    if (cidr == NULL || gateway == NULL || verb == NULL) {
        return -1;
    }

    add = (strcmp(verb, "replace") == 0 || strcmp(verb, "add") == 0);

    if (strlen(cidr) >= sizeof(cidr_buf)) {
        return -1;
    }
    snprintf(cidr_buf, sizeof(cidr_buf), "%s", cidr);
    slash = strchr(cidr_buf, '/');
    if (slash == NULL || slash == cidr_buf) {
        return -1;
    }
    *slash = '\0';

    if (inet_pton(AF_INET, cidr_buf, &dst_addr) != 1) {
        return -1;
    }
    if (inet_pton(AF_INET, gateway, &gw_addr) != 1) {
        return -1;
    }

    errno = 0;
    prefix = strtol(slash + 1, &end, 10);
    if (errno != 0 || end == slash + 1 || *end != '\0' ||
        prefix < 0 || prefix > 32) {
        return -1;
    }

    netmask_val = (prefix == 0) ? 0U :
                  htonl(~0U << (32U - (uint32_t)prefix));

    memset(&rt, 0, sizeof(rt));
    dst = (struct sockaddr_in *)&rt.rt_dst;
    dst->sin_family = AF_INET;
    dst->sin_addr.s_addr = dst_addr.s_addr & netmask_val;

    gw = (struct sockaddr_in *)&rt.rt_gateway;
    gw->sin_family = AF_INET;
    gw->sin_addr.s_addr = gw_addr.s_addr;

    msk = (struct sockaddr_in *)&rt.rt_genmask;
    msk->sin_family = AF_INET;
    msk->sin_addr.s_addr = netmask_val;

    rt.rt_flags = RTF_UP | RTF_GATEWAY;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        print_errno_message(LOG_ERR, "socket for route");
        return -1;
    }

    ret = ioctl(sock, add ? SIOCADDRT : SIOCDELRT, &rt);
    close(sock);

    if (ret < 0) {
        if (errno == EEXIST && add) {
            return 0;
        }
        log_message(LOG_ERR, "Failed to %s route %s via %s: %s",
                    verb, cidr, gateway, strerror(errno));
        return -1;
    }

    return 0;
}
#endif

int camex_add_route(const char *cidr, const char *gateway)
{
    return run_ip_route("replace", cidr, gateway);
}

int camex_del_route(const char *cidr, const char *gateway)
{
    return run_ip_route("del", cidr, gateway);
}
