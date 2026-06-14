/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * camex.c — client/server UDP/TUN tunnel (main entry point)
 *
 */

#define _GNU_SOURCE

#include "camex.h"
#include "client.h"
#include "server.h"
#include "config.h"
#include "crypto.h"
#include "log.h"
#include "main.h"
#include "net.h"
#include "proto.h"
#include "tun.h"
#include "util.h"
#include "version.h"

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#if defined(__APPLE__)
#include <sys/types.h>
#endif
#include <net/route.h>
#include <net/if_arp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/mman.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <sys/time.h>
#include <sys/syslog.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Global state */
camex_config_t current_config;
volatile sig_atomic_t running = 1;
volatile sig_atomic_t reload_config = 0;
time_t g_now = 0;
uint8_t client_mode = 0;
uint8_t server_mode = 0;

time_t client_reconnect_at = 0;
uint8_t client_link_up = 0;

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

    if (get_random_bytes(client_state.send_nonce_prefix,
                         sizeof(client_state.send_nonce_prefix)) != 0) {
        log_message(LOG_WARNING,
                    "Failed to generate nonce prefix; using zeros");
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
            }
            if (!current_config.encrypt && g->encrypt_set) {
                current_config.encrypt = g->encrypt;
            }
            if (current_config.mtu == 1500 && g->mtu > 0) {
                current_config.mtu = g->mtu;
            }
        }

        if (current_config.port <= 0) {
            log_message(LOG_ERR,
                "Server port must be specified (use -p or port= in [server])");
            return -1;
        }
    }

    if (current_config.encrypt) {
        if (crypto_init(current_config.psk) != 0) {
            return -1;
        }
        crypto_wipe(current_config.psk, sizeof(current_config.psk));
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

        if (current_config.auto_config) {
            if (client_send_register() != 0) {
                log_message(LOG_WARNING, "Initial auto registration failed");
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

        if (tun_create_device(local_ip, local_netmask, current_config.mtu,
                              current_config.tun_dev) != 0) {
            net_close();
            return -1;
        }

        if (!current_config.auto_config && client_send_register() != 0) {
            log_message(LOG_WARNING, "Initial client registration failed");
        }

        client_link_up = 1U;
        log_message(LOG_NOTICE, "Connection to server %s:%d established",
                    current_config.server_host, current_config.port);
        log_message(LOG_INFO, "Tunnel initialized");
        log_message(LOG_INFO, "  Mode: %s", mode_to_string(current_config.mode));
        log_message(LOG_INFO, "  Server: %s:%d",
                    current_config.server_host, current_config.port);
        log_message(LOG_INFO, "  Local CIDR: %s", current_config.local_cidr);
        log_message(LOG_INFO, "  Tunnel device: %s", tun_name);
        log_message(LOG_INFO, "  MTU: %d", current_config.mtu);
        log_message(LOG_INFO, "  Encryption: %s",
                    current_config.encrypt ? "enabled" : "disabled");
        log_message(LOG_INFO, "  Transport: %s",
                    current_config.transport == CAMEX_TRANSPORT_TCP
                        ? "TCP" : "UDP");
        return 0;
    }

    config_build_keystore();

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

        if (tun_create_device(local_ip, local_netmask, current_config.mtu,
                              current_config.tun_dev) != 0) {
            net_close();
            return -1;
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
    log_message(LOG_INFO, "  Encryption: %s",
                current_config.encrypt ? "enabled" : "disabled");
    log_message(LOG_INFO, "  Transport: %s",
                current_config.transport == CAMEX_TRANSPORT_TCP
                    ? "TCP" : "UDP");
    return 0;
}

static void drain_udp_packets(void)
{
    uint8_t buffer[TUN_PACKET_MAX + CAMEX_HDR_LEN + 1 + 16U];
    struct sockaddr_in from;
    int len;

    if (server_mode) {
        while (1) {
            socklen_t fromlen = sizeof(from);

            len = recvfrom(net_fd, buffer, sizeof(buffer), 0,
                           (struct sockaddr *)&from, &fromlen);
            if (len > 0) {
                if (server_handle_packet(buffer, (size_t)len, &from) != 0) {
                    char peer[64];
                    net_sockaddr_to_string(&from, peer, sizeof(peer));
                    log_message(LOG_WARNING, "Dropped packet from %s", peer);
                }
                continue;
            }

            if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }

            break;
        }
        return;
    }

    /* Client mode */
    if (current_config.transport == CAMEX_TRANSPORT_TCP) {
        size_t frame_len;
        if (net_tcp_recv_frame(net_fd, buffer, sizeof(buffer),
                               &frame_len) == 0) {
            client_state.last_recv = g_now;
            if (client_handle_net_packet(buffer, frame_len) != 0) {
                log_message(LOG_WARNING, "Dropped packet from server");
            }
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            log_message(LOG_WARNING,
                        "TCP read error from server — will reconnect");
            net_close();
        }
        return;
    }

    /* UDP client: drain all available packets */
    while (1) {
        len = recv(net_fd, buffer, sizeof(buffer), 0);
        if (len > 0) {
            client_state.last_recv = g_now;
            if (client_handle_net_packet(buffer, (size_t)len) != 0) {
                log_message(LOG_WARNING, "Dropped packet from server");
            }
            continue;
        }

        if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
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
            break;
        }

        if (set_fd_nonblocking(client_fd) != 0) {
            close(client_fd);
            continue;
        }

        for (i = 0; i < CAMEX_MAX_CLIENTS; ++i) {
            if (!server_clients[i].active) {
                memset(&server_clients[i], 0, sizeof(server_clients[i]));
                server_clients[i].tcp_fd = client_fd;
                server_clients[i].addr = peer;
                server_clients[i].last_seen = g_now;
                server_clients[i].active = 1U;
                net_sockaddr_to_string(&peer, peer_str, sizeof(peer_str));
                log_message(LOG_INFO, "TCP client connected: %s (fd=%d)",
                            peer_str, client_fd);
                break;
            }
        }
        if (i >= CAMEX_MAX_CLIENTS) {
            close(client_fd);
            net_sockaddr_to_string(&peer, peer_str, sizeof(peer_str));
            log_message(LOG_WARNING,
                        "No free slots for TCP client: %s", peer_str);
        }
    }
}

static void drain_tcp_client_read(int fd)
{
    uint8_t buffer[TUN_PACKET_MAX + CAMEX_HDR_LEN + 1 + 16U];
    size_t frame_len;
    size_t i;
    char peer_str[64];

    if (net_tcp_recv_frame(fd, buffer, sizeof(buffer), &frame_len) != 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;  /* partial data; will re-select */
        }
        /* Connection closed or error — find and remove this client */
        for (i = 0; i < CAMEX_MAX_CLIENTS; ++i) {
            if (server_clients[i].active &&
                server_clients[i].tcp_fd == fd) {
                net_sockaddr_to_string(&server_clients[i].addr,
                                       peer_str, sizeof(peer_str));
                log_message(LOG_INFO,
                            "TCP client disconnected: %s (fd=%d)",
                            peer_str, fd);
                close(fd);
                memset(&server_clients[i], 0, sizeof(server_clients[i]));
                return;
            }
        }
        close(fd);
        return;
    }

    /* Find the client entry for this fd */
    for (i = 0; i < CAMEX_MAX_CLIENTS; ++i) {
        if (server_clients[i].active &&
            server_clients[i].tcp_fd == fd) {
            server_clients[i].last_seen = g_now;
            if (server_handle_packet(buffer, frame_len,
                                     &server_clients[i].addr) != 0) {
                net_sockaddr_to_string(&server_clients[i].addr,
                                       peer_str, sizeof(peer_str));
                log_message(LOG_WARNING, "Dropped TCP packet from %s",
                            peer_str);
            }
            return;
        }
    }
}

static void handle_tun_packet(void)
{
    uint8_t buffer[TUN_PACKET_MAX];
    int len;
    uint32_t src_ip_be = 0;
    uint32_t dst_ip_be = 0;

    while (1) {
        len = tun_read_packet(buffer, sizeof(buffer));
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
            if (rc != 0) {
                log_message(LOG_WARNING, "Dropped packet from tunnel device");
            }
            continue;
        }

        if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
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
    struct timeval tv;
    int maxfd;
    int ready;

    if (server_mode) {
        log_message(LOG_INFO, "Server started. Press Ctrl+C to stop.");
        while (running) {
            size_t si;

            g_now = time(NULL);
            FD_ZERO(&readset);

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

            if (tun_fd >= 0) {
                FD_SET(tun_fd, &readset);
                if (tun_fd > maxfd) {
                    maxfd = tun_fd;
                }
            }

            tv.tv_sec = 1;
            tv.tv_usec = 0;

            ready = select(maxfd + 1, &readset, NULL, NULL, &tv);
            if (ready > 0) {
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
                if (tun_fd >= 0 && FD_ISSET(tun_fd, &readset)) {
                    handle_tun_packet();
                }
            } else if (ready < 0 && errno != EINTR) {
                print_errno_message(LOG_ERR, "select");
            }

            server_expire_clients();
            if (reload_config) {
                camex_server_db_t saved_db = server_db;
                static camex_keystore_entry_t saved_keystore[CAMEX_MAX_CLIENTS + 1];
                size_t saved_keystore_count = server_keystore_count;

                reload_config = 0;
                log_message(LOG_INFO, "Reloading config: %s",
                            current_config.config_path);
                memcpy(saved_keystore, server_keystore,
                       sizeof(saved_keystore));
                if (config_db_load_file(current_config.config_path) != 0) {
                    server_db = saved_db;
                    memcpy(server_keystore, saved_keystore,
                           sizeof(saved_keystore));
                    server_keystore_count = saved_keystore_count;
                    log_message(LOG_WARNING,
                                "Keeping previous server configuration");
                } else {
                    config_build_keystore();
                }
            }
        }

        log_message(LOG_INFO, "Server stopped.");
        return;
    }

    log_message(LOG_INFO, "Client started. Press Ctrl+C to stop.");
    while (running) {
        g_now = time(NULL);

        if (net_fd < 0) {
            usleep(1000000);
            client_tick();
            continue;
        }

        FD_ZERO(&readset);
        FD_SET(net_fd, &readset);
        if (tun_fd >= 0) {
            FD_SET(tun_fd, &readset);
        }

        tv.tv_sec = 1;
        tv.tv_usec = 0;

        maxfd = net_fd;
        if (tun_fd > maxfd) {
            maxfd = tun_fd;
        }

        ready = select(maxfd + 1, &readset, NULL, NULL, &tv);
        if (ready > 0) {
            if (FD_ISSET(net_fd, &readset)) {
                drain_udp_packets();
            }
            if (tun_fd >= 0 && FD_ISSET(tun_fd, &readset)) {
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

    if (client_mode && current_config.gateway_ip[0] != '\0') {
        for (i = 0; i < current_config.route_count; ++i) {
            (void)camex_del_route(current_config.route_cidrs[i],
                                  current_config.gateway_ip);
        }
    }

    tun_close_device();
    net_close();
}

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

int camex_add_route(const char *cidr, const char *gateway)
{
    return run_ip_route("replace", cidr, gateway);
}

int camex_del_route(const char *cidr, const char *gateway)
{
    return run_ip_route("del", cidr, gateway);
}
