/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * server.c — server-side client tracking, packet forwarding, keystore lookup
 *
 */

#define _GNU_SOURCE

#include "server.h"
#include "client.h"
#include "crypto.h"
#include "log.h"
#include "util.h"
#include "proto.h"
#include "net.h"
#include "tun.h"
#include "config.h"
#include "camex.h"

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

server_client_t server_clients[CAMEX_MAX_CLIENTS];

server_client_t *server_find_by_ip(uint32_t ip_be)
{
    size_t i;

    for (i = 0; i < CAMEX_MAX_CLIENTS; ++i) {
        if (server_clients[i].active && server_clients[i].ip_be == ip_be) {
            return &server_clients[i];
        }
    }

    return NULL;
}

server_client_t *server_find_by_client_id(const char *client_id)
{
    size_t i;

    if (client_id == NULL || *client_id == '\0') {
        return NULL;
    }

    for (i = 0; i < CAMEX_MAX_CLIENTS; ++i) {
        if (server_clients[i].active &&
            strcasecmp(server_clients[i].client_id, client_id) == 0) {
            return &server_clients[i];
        }
    }

    return NULL;
}

server_client_t *server_find_by_addr(const struct sockaddr_in *addr)
{
    size_t i;

    if (addr == NULL) {
        return NULL;
    }

    for (i = 0; i < CAMEX_MAX_CLIENTS; ++i) {
        if (server_clients[i].active &&
            net_sockaddr_equal(&server_clients[i].addr, addr)) {
            return &server_clients[i];
        }
    }

    return NULL;
}

server_client_t *server_alloc_client(void)
{
    size_t i;
    size_t oldest = 0;
    time_t oldest_seen = 0;
    int oldest_found = 0;

    for (i = 0; i < CAMEX_MAX_CLIENTS; ++i) {
        if (!server_clients[i].active) {
            memset(&server_clients[i], 0, sizeof(server_clients[i]));
            (void)get_random_bytes(server_clients[i].send_nonce_prefix,
                                   sizeof(server_clients[i].send_nonce_prefix));
            server_clients[i].active = 1U;
            return &server_clients[i];
        }

        if (!oldest_found || server_clients[i].last_seen < oldest_seen) {
            oldest_found = 1;
            oldest = i;
            oldest_seen = server_clients[i].last_seen;
        }
    }

    memset(&server_clients[oldest], 0, sizeof(server_clients[oldest]));
    (void)get_random_bytes(server_clients[oldest].send_nonce_prefix,
                           sizeof(server_clients[oldest].send_nonce_prefix));
    server_clients[oldest].active = 1U;
    return &server_clients[oldest];
}

server_client_t *server_upsert_client(uint32_t ip_be,
                                      const struct sockaddr_in *addr)
{
    server_client_t *entry = NULL;

    if (addr != NULL) {
        entry = server_find_by_addr(addr);
    }
    if (entry == NULL) {
        entry = server_find_by_ip(ip_be);
    }
    if (entry == NULL) {
        entry = server_alloc_client();
    }
    if (entry == NULL) {
        return NULL;
    }

    entry->ip_be = ip_be;
    if (addr != NULL) {
        entry->addr = *addr;
    }
    entry->last_seen = g_now;
    return entry;
}

void server_expire_clients(void)
{
    static time_t last_expire_check = 0;
    size_t i;
    time_t now = (g_now != 0) ? g_now : time(NULL);

    if (now == last_expire_check) {
        return;
    }
    last_expire_check = now;

    for (i = 0; i < CAMEX_MAX_CLIENTS; ++i) {
        if (server_clients[i].active &&
            difftime(now, server_clients[i].last_seen) >
                (double)CAMEX_CLIENT_TIMEOUT) {
            char ipbuf[INET_ADDRSTRLEN];
            struct in_addr addr;

            addr.s_addr = server_clients[i].ip_be;
            inet_ntop(AF_INET, &addr, ipbuf, sizeof(ipbuf));
            log_message(LOG_INFO, "Expired client %s", ipbuf);
            memset(&server_clients[i], 0, sizeof(server_clients[i]));
        }
    }
}

void server_reset_replay(server_client_t *entry)
{
    if (entry == NULL) {
        return;
    }

    entry->recv_seq_max = 0;
    entry->recv_window = 0;
}

int server_send_config_response(const struct sockaddr_in *from,
                                server_client_t *entry,
                                const char *client_id)
{
    char message[CAMEX_CONTROL_MAX];
    uint8_t packet[CAMEX_HDR_LEN + 1 + CAMEX_CONTROL_MAX + 16U];
    size_t packet_len = 0U;
    camex_profile_t profile;

    if (from == NULL) {
        return -1;
    }

    if (config_db_lookup_profile(client_id, &profile) != 0) {
        log_message(LOG_ERR, "Missing config for client %s",
                    client_id != NULL ? client_id : "(null)");
        return -1;
    }

    if (profile.local_cidr[0] == '\0' || profile.gateway_ip[0] == '\0') {
        log_message(LOG_ERR, "Incomplete config for client %s",
                    client_id != NULL ? client_id : "(null)");
        return -1;
    }

    if (proto_build_config(client_id, &profile,
                           message, sizeof(message)) != 0) {
        return -1;
    }

    if (current_config.encrypt) {
        if (entry == NULL) {
            entry = server_upsert_client(0U, from);
        }
        if (entry == NULL) {
            return -1;
        }
        {
            uint64_t seq = entry->send_seq;
            if (crypto_encrypt_packet(CAMEX_PACKET_CONFIG, seq,
                    entry->send_nonce_prefix,
                    is_zero_key(entry->psk_key) ? NULL : entry->psk_key,
                    (const uint8_t *)message, strlen(message),
                    packet, sizeof(packet), &packet_len) != 0) {
                return -1;
            }
            entry->send_seq = seq + 1U;
        }
        return net_send_payload(net_fd, &entry->addr, packet, packet_len);
    }

    return net_send_text(net_fd, from, message);
}

int server_handle_plain_register(const uint8_t *buffer, size_t len,
                                 const struct sockaddr_in *from,
                                 const uint8_t *used_key)
{
    char message[CAMEX_CONTROL_MAX];
    char local_ip[16];
    char client_id[CAMEX_CLIENT_ID_LEN];
    uint8_t auto_request = 0U;
    server_client_t *entry;
    struct in_addr addr;

    if (buffer == NULL || from == NULL || len == 0U) {
        return -1;
    }

    if (len >= sizeof(message)) {
        len = sizeof(message) - 1U;
    }

    memcpy(message, buffer, len);
    message[len] = '\0';
    local_ip[0] = '\0';
    client_id[0] = '\0';

    if (proto_parse_register(message, client_id, sizeof(client_id),
                             local_ip, sizeof(local_ip),
                             &auto_request) != 0) {
        return -1;
    }

    /* Rate-limit: reject repeated REGISTER from same addr within 5 seconds */
    {
        server_client_t *existing = server_find_by_addr(from);
        if (existing != NULL && existing->last_register_time != 0 &&
            difftime(g_now, existing->last_register_time) < 5.0) {
            return 0;
        }
    }

    if (auto_request) {
        entry = server_find_by_client_id(client_id);
        if (entry != NULL) {
            if (!net_sockaddr_equal(&entry->addr, from)) {
                char old_peer[64], new_peer[64];
                net_sockaddr_to_string(&entry->addr, old_peer, sizeof(old_peer));
                net_sockaddr_to_string(from, new_peer, sizeof(new_peer));
                log_message(LOG_INFO, "Client %s reconnected: %s -> %s",
                            client_id, old_peer, new_peer);
            }
            entry->addr = *from;
            entry->last_seen = g_now;
        } else {
            entry = server_alloc_client();
            if (entry == NULL) {
                log_message(LOG_WARNING, "No free slots for client %s",
                            client_id);
                return -1;
            }
            entry->addr = *from;
            entry->last_seen = g_now;
        }
        snprintf(entry->client_id, sizeof(entry->client_id), "%s", client_id);
        server_reset_replay(entry);
        entry->last_register_time = g_now;
        if (used_key != NULL) {
            memcpy(entry->psk_key, used_key, 32);
        }
        if (server_send_config_response(from, entry, client_id) != 0) {
            return -1;
        }
        log_message(LOG_INFO, "Registered auto client %s", client_id);
        return 0;
    }

    if (inet_pton(AF_INET, local_ip, &addr) != 1) {
        log_message(LOG_WARNING, "Rejected invalid client IP: %s", local_ip);
        return -1;
    }

    {
        server_client_t *by_addr = server_find_by_addr(from);
        server_client_t *by_ip   = server_find_by_ip(addr.s_addr);

        if (by_addr != NULL) {
            by_addr->ip_be = addr.s_addr;
            by_addr->last_seen = g_now;
            by_addr->last_register_time = g_now;
            if (used_key != NULL) {
                memcpy(by_addr->psk_key, used_key, 32);
            }
            return 0;
        }

        if (by_ip != NULL) {
            char old_peer[64], new_peer[64];
            net_sockaddr_to_string(&by_ip->addr, old_peer, sizeof(old_peer));
            net_sockaddr_to_string(from, new_peer, sizeof(new_peer));
            log_message(LOG_INFO, "Client %s reconnected: %s -> %s",
                        local_ip, old_peer, new_peer);
            by_ip->addr = *from;
            by_ip->last_seen = g_now;
            by_ip->last_register_time = g_now;
            server_reset_replay(by_ip);
            if (used_key != NULL) {
                memcpy(by_ip->psk_key, used_key, 32);
            }
            return 0;
        }

        entry = server_alloc_client();
        if (entry == NULL) {
            log_message(LOG_WARNING, "No free slots for client %s", local_ip);
            return -1;
        }
        entry->ip_be = addr.s_addr;
        entry->addr = *from;
        entry->last_seen = g_now;
        entry->last_register_time = g_now;
        server_reset_replay(entry);
        if (used_key != NULL) {
            memcpy(entry->psk_key, used_key, 32);
        }
        log_message(LOG_INFO, "Registered client %s", local_ip);
        return 0;
    }
}

const uint8_t *server_try_all_keys(const uint8_t *buffer, size_t len,
                                   uint8_t *type,
                                   uint8_t *plain, size_t plain_size,
                                   size_t *plain_len, uint64_t *seq)
{
    size_t i;

    for (i = 0U; i < server_keystore_count; ++i) {
        if (crypto_decrypt_packet(buffer, len, type, plain, plain_size,
                                  plain_len, seq,
                                  server_keystore[i].psk_key) == 0) {
            return server_keystore[i].psk_key;
        }
    }
    return NULL;
}

int server_forward_packet(const uint8_t *packet, size_t len,
                          uint32_t src_ip_be, uint32_t dst_ip_be)
{
    server_client_t *dst;
    uint8_t encrypted[TUN_PACKET_MAX + CAMEX_HDR_LEN + 1 + 16U];
    size_t encrypted_len = 0;

    (void)src_ip_be;

    dst = server_find_by_ip(dst_ip_be);
    if (dst == NULL) {
        if (tun_fd >= 0) {
            return tun_write_packet(packet, len);
        }
        return -1;
    }

    dst->last_seen = (g_now != 0) ? g_now : time(NULL);

    if (current_config.encrypt) {
        if (crypto_encrypt_packet(CAMEX_PACKET_DATA, dst->send_seq++,
                dst->send_nonce_prefix,
                is_zero_key(dst->psk_key) ? NULL : dst->psk_key,
                packet, len, encrypted, sizeof(encrypted),
                &encrypted_len) != 0) {
            return -1;
        }
        return net_send_payload(net_fd, &dst->addr, encrypted, encrypted_len);
    }

    return net_send_payload(net_fd, &dst->addr, packet, len);
}

int server_handle_packet(const uint8_t *buffer, size_t len,
                         const struct sockaddr_in *from)
{
    uint8_t plain[TUN_PACKET_MAX];
    uint8_t type = 0U;
    size_t plain_len = 0U;
    uint64_t seq = 0;
    uint32_t src_ip_be = 0;
    uint32_t dst_ip_be = 0;
    server_client_t *src;
    server_client_t *decrypt_src = NULL;
    const uint8_t *used_key = NULL;

    if (buffer == NULL || from == NULL || len == 0U) {
        return -1;
    }

    if (current_config.encrypt) {
        src = server_find_by_addr(from);

        if (src != NULL && !is_zero_key(src->psk_key)) {
            if (crypto_decrypt_packet(buffer, len, &type, plain,
                                      sizeof(plain), &plain_len,
                                      &seq, src->psk_key) == 0) {
                used_key = src->psk_key;
                decrypt_src = src;
            }
        }

        if (used_key == NULL) {
            char peer[64];
            net_sockaddr_to_string(from, peer, sizeof(peer));
            log_message(LOG_WARNING,
                        "Crypto error from %s: decrypt failed, "
                        "trying all %zu keystore keys",
                        peer, server_keystore_count);

            used_key = server_try_all_keys(buffer, len, &type, plain,
                                           sizeof(plain), &plain_len, &seq);
            if (used_key == NULL) {
                char peer_str[64];
                net_sockaddr_to_string(from, peer_str, sizeof(peer_str));
                log_message(LOG_WARNING,
                            "Dropped packet from %s: "
                            "decryption failed with all %zu keys "
                            "(type=%u, len=%zu)",
                            peer_str, server_keystore_count,
                            (unsigned)(len > 4 ? buffer[4] : 0), len);
                return -1;
            }
            decrypt_src = server_find_by_addr(from);
        }

        if (type == CAMEX_PACKET_REGISTER) {
            return server_handle_plain_register(plain, plain_len, from,
                                                used_key);
        }

        if (type == CAMEX_PACKET_CONFIG) {
            return 0;
        }

        if (type != CAMEX_PACKET_DATA) {
            char peer[64];
            net_sockaddr_to_string(from, peer, sizeof(peer));
            log_message(LOG_WARNING,
                        "Dropped packet from %s: unknown type %u",
                        peer, (unsigned)type);
            return -1;
        }

        buffer = plain;
        len = plain_len;
    }

    if (!current_config.encrypt && len >= 4U &&
        memcmp(buffer, CAMEX_MAGIC, 4) == 0) {
        server_handle_plain_register(buffer, len, from, NULL);
        return 0;
    }

    if (ipv4_parse_endpoints(buffer, len, &src_ip_be, &dst_ip_be) != 0) {
        return 0;
    }

    src = server_upsert_client(src_ip_be, from);
    if (src == NULL) {
        return -1;
    }

    if (current_config.encrypt) {
        if (decrypt_src == NULL && src != NULL) {
            server_reset_replay(src);
        }
        {
            server_client_t *check_entry = (decrypt_src != NULL)
                                           ? decrypt_src : src;
            if (replay_check(&check_entry->recv_seq_max,
                             &check_entry->recv_window, seq) != 0) {
                char peer[64];
                net_sockaddr_to_string(from, peer, sizeof(peer));
                log_message(LOG_WARNING,
                            "Dropped packet from %s: replay check failed "
                            "(seq=%" PRIu64 ", max=%" PRIu64 ")",
                            peer, seq, check_entry->recv_seq_max);
                return -1;
            }
        }
    }

    return server_forward_packet(buffer, len, src_ip_be, dst_ip_be);
}

int server_socket_create(const char *bind_ip, int port,
                         const char *bind_dev)
{
    struct sockaddr_in addr;
    int reuse = 1;
    int fd;
    (void)bind_dev;

    fd = net_open_udp_socket();
    if (fd < 0) {
        return -1;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse, sizeof(reuse)) < 0) {
        print_errno_message(LOG_WARNING, "setsockopt(SO_REUSEADDR)");
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind_ip != NULL && *bind_ip != '\0' &&
        inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) {
        log_message(LOG_ERR, "Invalid bind IP: %s", bind_ip);
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        print_errno_message(LOG_ERR, "bind");
        close(fd);
        return -1;
    }

    net_fd = fd;
    return 0;
}
