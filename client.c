/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * client.c — client-side state machine (register, reconnect, packet handling)
 *
 */

#define _GNU_SOURCE

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
#include <stdio.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

client_state_t client_state;

void client_state_init(client_state_t *state)
{
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
}

int client_send_register(void)
{
    uint8_t packet[CAMEX_HDR_LEN + 1 + 16U + CAMEX_CONTROL_MAX];
    char message[CAMEX_CONTROL_MAX];
    size_t packet_len = 0;
    size_t message_len;

    if (proto_build_register(&current_config, message,
                             sizeof(message)) != 0) {
        return -1;
    }

    message_len = strlen(message);
    if (current_config.encrypt) {
        uint64_t seq = client_state.send_seq;
        if (crypto_encrypt_packet(CAMEX_PACKET_REGISTER, seq,
                client_state.send_nonce_prefix,
                NULL,
                (const uint8_t *)message, message_len,
                packet, sizeof(packet), &packet_len) != 0) {
            return -1;
        }
        client_state.send_seq = seq + 1U;
        if (net_client_send(packet, packet_len) != 0) {
            return -1;
        }
    } else {
        if (current_config.transport == CAMEX_TRANSPORT_TCP) {
            if (net_tcp_send_frame(net_fd, (const uint8_t *)message,
                                   message_len) != 0) {
                return -1;
            }
        } else {
            if (net_send_text(net_fd, NULL, message) != 0) {
                return -1;
            }
        }
    }

    client_state.last_register = g_now;
    client_state.registered = 1U;
    return 0;
}

int client_handle_net_packet(const uint8_t *buffer, size_t len)
{
    uint8_t plain[TUN_PACKET_MAX];
    uint8_t type = 0U;
    size_t plain_len = 0U;
    uint64_t seq = 0;

    if (buffer == NULL || len == 0U) {
        return -1;
    }

    if (current_config.encrypt) {
        if (crypto_decrypt_packet(buffer, len, &type, plain,
                                  sizeof(plain), &plain_len, &seq,
                                  NULL) != 0) {
            char peer[64];
            net_sockaddr_to_string(&server_addr, peer, sizeof(peer));
            log_message(LOG_WARNING,
                        "Crypto error: failed to decrypt packet "
                        "from server %s (len=%zu)",
                        peer, len);
            return -1;
        }

        if (type == CAMEX_PACKET_CONFIG && current_config.auto_config) {
            if (replay_check(&client_state.recv_seq_max,
                             &client_state.recv_window, seq) != 0) {
                log_message(LOG_WARNING,
                            "Replay check failed for CONFIG packet "
                            "(seq=%" PRIu64 ", max=%" PRIu64 ")",
                            seq, client_state.recv_seq_max);
                return -1;
            }
            return client_apply_config_response(plain, plain_len);
        }

        if (type != CAMEX_PACKET_DATA) {
            return 0;
        }

        if (replay_check(&client_state.recv_seq_max,
                         &client_state.recv_window, seq) != 0) {
            log_message(LOG_WARNING,
                        "Replay check failed for DATA packet "
                        "(seq=%" PRIu64 ", max=%" PRIu64 ")",
                        seq, client_state.recv_seq_max);
            return -1;
        }

        buffer = plain;
        len = plain_len;
    } else if (current_config.auto_config && len >= 4U &&
               memcmp(buffer, CAMEX_MAGIC, 4) == 0) {
        return client_apply_config_response(buffer, len);
    }

    return tun_write_packet(buffer, len);
}

int client_wait_for_config(void)
{
    uint8_t buffer[CAMEX_CONTROL_MAX];
    fd_set readset;
    struct timeval tv;
    int ready;
    unsigned int probe = 0;

    log_message(LOG_INFO, "Waiting for auto-config from server...");

    unsigned int wait_retries;
    for (wait_retries = 0; wait_retries < 300; wait_retries++) {
        if (!running) {
            return -1;
        }

        FD_ZERO(&readset);
        FD_SET(net_fd, &readset);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        ready = select(net_fd + 1, &readset, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            print_errno_message(LOG_ERR, "select");
            return -1;
        }

        if (ready == 0 || !FD_ISSET(net_fd, &readset)) {
            ++probe;
            if (probe % 10 == 0) {
                log_message(LOG_INFO,
                            "Still waiting for server config (%us)...", probe);
            }
            if (client_send_register() != 0) {
                return -1;
            }
            continue;
        }

        if (current_config.transport == CAMEX_TRANSPORT_TCP) {
            size_t frame_len;
            if (net_tcp_recv_frame(net_fd, buffer, sizeof(buffer),
                                   &frame_len) != 0) {
                continue;
            }
            ready = (int)frame_len;
        } else {
            ready = recv(net_fd, buffer, sizeof(buffer), 0);
            if (ready <= 0) {
                continue;
            }
        }
        if (client_handle_net_packet(buffer, (size_t)ready) == 0 &&
            current_config.local_cidr[0] != '\0' &&
            current_config.gateway_ip[0] != '\0') {
            return 0;
        }
    }

    log_message(LOG_ERR, "Timeout: server did not provide config after 300 retries");
    return -1;
}

int client_apply_config_response(const uint8_t *payload, size_t payload_len)
{
    char message[CAMEX_CONTROL_MAX];
    int rc;

    if (payload == NULL || payload_len >= sizeof(message)) {
        log_message(LOG_ERR,
                    "Config response too large: %zu bytes (max %zu)",
                    payload_len, sizeof(message) - 1U);
        return -1;
    }

    memcpy(message, payload, payload_len);
    message[payload_len] = '\0';
    rc = proto_parse_config(message, &current_config);
    if (rc == 0) {
        char lip[16], lmask[16];

        client_state.config_received = 1U;
        if (parse_local_cidr(current_config.local_cidr, lip, sizeof(lip),
                             lmask, sizeof(lmask)) == 0) {
            snprintf(current_config.local_ip, sizeof(current_config.local_ip),
                     "%s", lip);
        }
        log_message(LOG_INFO,
                    "Received config from server:"
                    " CIDR=%s GW=%s MTU=%d routes=%u",
                    current_config.local_cidr,
                    current_config.gateway_ip,
                    current_config.mtu,
                    (unsigned)current_config.route_count);
    }

    return rc;
}

int client_handle_tun_packet(const uint8_t *buffer, size_t len)
{
    uint8_t packet[TUN_PACKET_MAX + CAMEX_HDR_LEN + 1 + 16U];
    size_t packet_len = 0;

    if (buffer == NULL || len == 0U || len > TUN_PACKET_MAX) {
        return -1;
    }

    if (current_config.encrypt) {
        uint64_t seq = client_state.send_seq;
        if (crypto_encrypt_packet(CAMEX_PACKET_DATA, seq,
                client_state.send_nonce_prefix,
                NULL, buffer, len,
                packet, sizeof(packet), &packet_len) != 0) {
            return -1;
        }
        client_state.send_seq = seq + 1U;
        return net_client_send(packet, packet_len);
    }

    return net_client_send(buffer, len);
}

static unsigned int reconnect_backoff = 0;

void client_reconnect(void)
{
    net_close();

    log_message(LOG_INFO, "Reconnecting to server %s:%d...",
                current_config.server_host, current_config.port);

    if (client_socket_create(current_config.server_host,
                             current_config.port,
                             current_config.bind_dev) != 0) {
        unsigned int delay = CAMEX_RECONNECT_INTERVAL << reconnect_backoff;
        if (delay > 60) {
            delay = 60;
        }
        if (reconnect_backoff < 4) {
            reconnect_backoff++;
        }
        log_message(LOG_WARNING, "Reconnect failed; will retry in %us", delay);
        client_reconnect_at = g_now + (time_t)delay;
        client_state.last_recv = 0;
        return;
    }

    reconnect_backoff = 0;

    client_state.registered = 0;
    client_state.last_register = 0;
    client_state.config_received = 0;
    client_state.last_recv = g_now;
    client_reconnect_at = 0;

    if (client_send_register() != 0) {
        log_message(LOG_WARNING, "Failed to send register after reconnect");
    } else {
        log_message(LOG_NOTICE, "Connection to server %s:%d restored",
                    current_config.server_host, current_config.port);
        client_link_up = 1U;
        if (current_config.auto_config) {
            log_message(LOG_INFO,
                        "Auto-config mode: waiting for server config response");
        }
    }
}

void client_tick(void)
{
    if (net_fd < 0 || client_reconnect_at > 0) {
        if (client_reconnect_at == 0 ||
            difftime(g_now, client_reconnect_at) >= 0.0) {
            client_reconnect();
        }
        return;
    }

    if (current_config.auto_config &&
        client_state.last_recv > 0 &&
        difftime(g_now, client_state.last_recv) >=
            (double)CAMEX_SERVER_TIMEOUT) {
        log_message(LOG_ERR,
                    "Connection lost: no data from server %s:%d for %ds",
                    current_config.server_host, current_config.port,
                    CAMEX_SERVER_TIMEOUT);
        client_link_up = 0U;
        client_state.last_recv = 0;
        client_reconnect();
        return;
    }

    if (!client_state.registered ||
        difftime(g_now, client_state.last_register) >=
            (double)CAMEX_REGISTER_INTERVAL) {
        if (client_send_register() != 0) {
            log_message(LOG_ERR,
                "Failed to send REGISTER to server %s:%d — forcing reconnect",
                current_config.server_host, current_config.port);
            net_close();
            client_reconnect_at = 0;
        }
    }
}

int client_socket_create(const char *host, int port, const char *bind_dev)
{
    int fd;
    (void)bind_dev;

    if (current_config.transport == CAMEX_TRANSPORT_TCP) {
        fd = net_tcp_connect(host, port);
        if (fd < 0) {
            return -1;
        }
        net_fd = fd;
        return 0;
    }

    /* UDP path */
    if (net_resolve_endpoint(host, port, &server_addr, "server",
                             SOCK_DGRAM) != 0) {
        return -1;
    }

    fd = net_open_udp_socket();
    if (fd < 0) {
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&server_addr,
                sizeof(server_addr)) < 0) {
        print_errno_message(LOG_ERR, "connect");
        close(fd);
        return -1;
    }

    net_fd = fd;
    return 0;
}
