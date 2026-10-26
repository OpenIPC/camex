/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * client.c — client side of protocol v2: handshake, sessions, reconnect
 *
 */

#include "client.h"
#include "crypto.h"
#include "log.h"
#include "util.h"
#include "proto.h"
#include "net.h"
#include "packet.h"
#include "tun.h"
#include "config.h"
#include "camex.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#ifndef _WIN32
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#define close(fd) closesocket(fd)
#endif

/* Exponential backoff for reconnection attempts (seconds). */
static unsigned int reconnect_backoff = 0;

client_state_t client_state;
tcp_recv_state_t client_recv_state;

void client_state_init(client_state_t *state)
{
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
}

void client_wipe(void)
{
    noise_wipe(&client_state.hs);
    session_wipe(&client_state.cur);
    session_wipe(&client_state.prev);
    client_state.hs_pending = 0U;
}

static int client_send_init(void)
{
    const camex_crypto_t *ctx = crypto_get_ctx();
    uint8_t payload[V2_TIMESTAMP_LEN + CAMEX_CONTROL_MAX];
    uint8_t packet[V2_INIT_HDR + NOISE_MSG1_OVERHEAD + sizeof(payload) +
                   V2_MAC_LEN];
    uint8_t e_seed[NOISE_DHLEN];
    char message[CAMEX_CONTROL_MAX];
    size_t msg_len;
    size_t len;

    if (!ctx->has_server_pub || !ctx->has_global) {
        log_message(LOG_ERR, "Missing the server public key or the PSK");
        return -1;
    }
    if (proto_build_register(&current_config, message, sizeof(message)) != 0) {
        return -1;
    }
    msg_len = strlen(message);
    v2_timestamp(payload);
    memcpy(payload + V2_TIMESTAMP_LEN, message, msg_len);

    if (get_random_bytes(e_seed, sizeof(e_seed)) != 0) {
        return -1;
    }
    /* A new ephemeral key and index for every INIT */
    noise_wipe(&client_state.hs);
    noise_init_initiator(&client_state.hs,
                         (const uint8_t *)CAMEX_NOISE_PROLOGUE,
                         sizeof(CAMEX_NOISE_PROLOGUE) - 1U, ctx->server_pub);
    client_state.hs_index = v2_new_index();
    if (client_state.hs_index == 0U) {
        crypto_wipe(e_seed, sizeof(e_seed));
        return -1;
    }
    memcpy(packet, CAMEX_MAGIC, 4);
    packet[4] = V2_TYPE_INIT;
    v2_write_u32(packet + 5, client_state.hs_index);
    len = noise_write_msg1(&client_state.hs, ctx->psk_key, e_seed, payload,
                           V2_TIMESTAMP_LEN + msg_len, packet + V2_INIT_HDR,
                           sizeof(packet) - V2_INIT_HDR - V2_MAC_LEN);
    crypto_wipe(e_seed, sizeof(e_seed));
    if (len == 0U) {
        return -1;
    }
    len += V2_INIT_HDR;
    v2_mac1(ctx->server_pub, packet, len, packet + len);
    len += V2_MAC_LEN;
    client_state.hs_pending = 1U;
    client_state.hs_sent = g_now;
    return net_client_send(packet, len) < 0 ? -1 : 0;
}

int client_start_handshake(void)
{
    client_state.hs_started = g_now;
    return client_send_init();
}

/* A later handshake may bring a different CONFIG (the server's profile
 * changed): it is applied to the live tunnel (#49) */
static void client_check_config_change(const uint8_t *payload, size_t len)
{
    char message[CAMEX_CONTROL_MAX];
    camex_config_t probe = current_config;

    if (len >= sizeof(message)) {
        return;
    }
    memcpy(message, payload, len);
    message[len] = '\0';
    if (proto_parse_config(message, &probe) != 0) {
        return;
    }
    if (camex_client_reconfig_pending() ||
        strcmp(probe.local_cidr, current_config.local_cidr) != 0 ||
        strcmp(probe.gateway_ip, current_config.gateway_ip) != 0 ||
        probe.mtu != current_config.mtu ||
        probe.route_count != current_config.route_count ||
        memcmp(probe.route_cidrs, current_config.route_cidrs,
               sizeof(probe.route_cidrs)) != 0) {
        /* Applied to the live tunnel (#49); on failure the old settings
         * stay and the next handshake tries again (a changed CONFIG, or
         * camex_client_reconfig_pending()) */
        (void)camex_client_reconfigure(&probe);
    }
}

static int client_handle_resp(const uint8_t *buffer, size_t len)
{
    noise_hs_t hs;
    uint8_t payload[CAMEX_CONTROL_MAX];
    size_t payload_len = 0U;
    uint8_t c1[NOISE_KEYLEN];
    uint8_t c2[NOISE_KEYLEN];
    uint32_t server_index = v2_read_u32(buffer + 5);
    int first = !client_state.established;

    if (!client_state.hs_pending ||
        v2_read_u32(buffer + 9) != client_state.hs_index) {
        return -1;   /* not for our current handshake */
    }
    hs = client_state.hs;   /* keep the state if this RESP is bogus */
    if (noise_read_msg2(&hs, buffer + V2_RESP_HDR, len - V2_RESP_HDR,
                        payload, sizeof(payload) - 1U, &payload_len) != 0) {
        noise_wipe(&hs);
        log_peer(LOG_WARNING, "Handshake response from the server failed "
                 "to authenticate (wrong server key or PSK?)");
        return -1;
    }
    if (current_config.auto_config) {
        if (!client_state.config_received) {
            if (client_apply_config_response(payload, payload_len) != 0) {
                noise_wipe(&hs);
                return -1;
            }
        } else {
            client_check_config_change(payload, payload_len);
        }
    }
    noise_split(&hs, c1, c2);
    noise_wipe(&hs);
    noise_wipe(&client_state.hs);
    client_state.hs_pending = 0U;

    /* The old session keeps receiving for a while: packets in flight */
    session_wipe(&client_state.prev);
    if (client_state.cur.valid) {
        client_state.prev = client_state.cur;
        client_state.prev_until = g_now + V2_PREV_SESSION_GRACE;
    }
    session_init(&client_state.cur, client_state.hs_index, server_index,
                 c1, c2, g_now);
    crypto_wipe(c1, sizeof(c1));
    crypto_wipe(c2, sizeof(c2));

    client_state.last_recv = g_now;
    client_state.established = 1U;
    client_state.confirm_pending = 1U;
    reconnect_backoff = 0;
    /* The server sends on a new session only after the first packet on
     * it: confirm at once (and again in client_tick() until the server
     * answers on it) */
    {
        uint8_t ka[V2_OVERHEAD];
        size_t ka_len = session_seal(&client_state.cur, NULL, 0U, ka,
                                     sizeof(ka));

        if (ka_len > 0U) {
            (void)net_client_send(ka, ka_len);
            client_state.last_keepalive = g_now;
        }
    }
    if (first) {
        log_message(LOG_NOTICE, "Connection to server %s:%d established",
                    current_config.server_host, current_config.port);
    } else {
        log_message(LOG_INFO, "Session keys renewed");
    }
    return 0;
}

int client_handle_net_packet(const uint8_t *buffer, size_t len)
{
    uint8_t plain[TUN_PACKET_MAX];
    size_t plain_len = 0U;
    session_t *s = NULL;
    uint32_t idx;

    switch (v2_packet_type(buffer, len)) {
    case V2_TYPE_RESP:
        return client_handle_resp(buffer, len);
    case V2_TYPE_DATA:
        break;
    default:
        log_peer(LOG_WARNING, "Dropped packet from server: not a v2 packet "
                 "(len=%u)", (unsigned)len);
        return -1;
    }

    idx = v2_data_receiver(buffer);
    if (client_state.cur.valid && idx == client_state.cur.local_index) {
        s = &client_state.cur;
    } else if (client_state.prev.valid &&
               idx == client_state.prev.local_index &&
               g_now <= client_state.prev_until) {
        s = &client_state.prev;
    }
    if (s == NULL ||
        session_open(s, buffer, len, plain, sizeof(plain), &plain_len) != 0) {
        log_peer(LOG_WARNING, "Dropped packet from server: unknown session "
                 "or failed to authenticate (len=%u)", (unsigned)len);
        return -1;
    }
    client_state.last_recv = g_now;
    if (s == &client_state.cur) {
        client_state.confirm_pending = 0U;
    }
    reconnect_backoff = 0;
    if (plain_len == 0U) {
        return 0;   /* keepalive answer */
    }
    return packet_write(plain, plain_len);
}

/* While waiting for the config (#121): a new socket after a pause, the
 * name resolved again (the network may have changed: a new address, a
 * route, DNS), then a new handshake. 0 once one was sent, -1 when
 * stopped */
static int client_wait_reconnect(void)
{
    unsigned int attempt;

    for (attempt = 0U; ; ++attempt) {
        unsigned int step;

        net_close();
        memset(&client_recv_state, 0, sizeof(client_recv_state));
        for (step = 0U; step < (unsigned int)CAMEX_RECONNECT_INTERVAL &&
                        running; ++step) {
            struct timeval pause = { 1, 0 };

            select(0, NULL, NULL, NULL, &pause);
        }
        if (!running) {
            return -1;
        }
        if (client_socket_create(current_config.server_host,
                                 current_config.port,
                                 current_config.bind_dev) != 0) {
            if (attempt % 12U == 0U) {
                log_message(LOG_WARNING, "Cannot reach server %s:%d, "
                            "retrying every %ds...",
                            current_config.server_host, current_config.port,
                            CAMEX_RECONNECT_INTERVAL);
            }
            continue;
        }
        g_now = time(NULL);
        if (client_start_handshake() == 0) {
            return 0;
        }
    }
}

int client_wait_for_config(void)
{
    uint8_t buffer[V2_RESP_HDR + NOISE_MSG2_OVERHEAD + CAMEX_CONTROL_MAX];
    fd_set readset;
    fd_set writeset;
    struct timeval tv;
    int ready;
    int maxfd;
    time_t wait_start;
    time_t socket_since;   /* this socket's first handshake */
    time_t next_note;
    unsigned int renewed = 0U;

    log_message(LOG_INFO, "Waiting for auto-config from server...");
    wait_start = time(NULL);
    socket_since = wait_start;
    next_note = wait_start + 10;

    /* As long as it takes (#121): a server or a network that is not there
     * yet is waited for, not given up on */
    for (;;) {
        if (!running) {
            return -1;
        }
        g_now = time(NULL);
        if (difftime(g_now, next_note) >= 0.0) {
            double waited = difftime(g_now, wait_start);

            log_message(LOG_INFO, "Still waiting for server config (%.0fs)...",
                        waited);
            /* Every 10 s for a minute, then every minute */
            next_note = g_now + (waited < 60.0 ? 10 : 60);
        }
        /* No answer on this socket for --server-timeout, or a handshake
         * that cannot be sent: the socket may hold a source address or a
         * route that is gone. Start over with a new one */
        if ((net_fd < 0 ||
             difftime(g_now, socket_since) >=
                 (double)current_config.server_timeout ||
             (client_state.hs_pending &&
              difftime(g_now, client_state.hs_sent) >= V2_HANDSHAKE_RETRY &&
              client_send_init() != 0))) {
            if (renewed++ % 12U == 0U) {
                log_message(LOG_INFO, "No answer from server %s:%d: a new "
                            "connection (and so every %ds)",
                            current_config.server_host, current_config.port,
                            current_config.server_timeout);
            }
            if (client_wait_reconnect() != 0) {
                return -1;
            }
            socket_since = time(NULL);
            continue;
        }

        FD_ZERO(&readset);
        FD_ZERO(&writeset);
        FD_SET(net_fd, &readset);
        /* A TCP INIT the socket could not take at once is sent here, as
         * in the main loop (a handshake after a reconnect included) */
        maxfd = net_tcp_pending_fdset(&writeset, net_fd);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        ready = select(maxfd + 1, &readset, &writeset, NULL, &tv);
        if (ready > 0) {
            net_tcp_flush_writable(&writeset);
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            print_errno_message(LOG_ERR, "select");
            return -1;
        }
        if (ready == 0 || !FD_ISSET(net_fd, &readset)) {
            continue;
        }

        if (current_config.transport == CAMEX_TRANSPORT_TCP) {
            size_t frame_len;
            int rc = net_tcp_recv_frame(net_fd, buffer, sizeof(buffer),
                                        &frame_len, &client_recv_state);
            if (rc < 0) {
                /* Closed before a CONFIG: the server had no address yet
                 * (its pool full, or it waits for returning clients after
                 * a restart, #81). Connect again within the same wait. */
                log_message(LOG_WARNING, "Connection to the server closed "
                            "before its config; reconnecting");
                if (client_wait_reconnect() != 0) {
                    return -1;
                }
                socket_since = time(NULL);
                continue;
            }
            if (rc != 0) {
                continue;
            }
            ready = (int)frame_len;
        } else {
            ready = recv(net_fd, (char *)buffer, sizeof(buffer), 0);
            if (ready <= 0) {
                continue;
            }
        }
        if (client_handle_net_packet(buffer, (size_t)ready) == 0) {
            /* The server answers on this socket (a packet that passed its
             * checks, not any packet): keep it */
            socket_since = time(NULL);
            if (client_state.config_received &&
                current_config.local_cidr[0] != '\0' &&
                current_config.gateway_ip[0] != '\0') {
                return 0;
            }
        }
    }
}

int client_apply_config_response(const uint8_t *payload, size_t payload_len)
{
    char message[CAMEX_CONTROL_MAX];
    int rc;

    if (payload == NULL || payload_len >= sizeof(message)) {
        log_message(LOG_ERR,
                    "Config response too large: %u bytes (max %u)",
                    (unsigned)payload_len,
                    (unsigned)(sizeof(message) - 1U));
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
    uint8_t packet[TUN_PACKET_MAX + V2_OVERHEAD];
    size_t packet_len;

    if (buffer == NULL || len == 0U || len > TUN_PACKET_MAX) {
        return -1;
    }
    if (!client_state.cur.valid) {
        return 0;   /* no session yet: the packet is dropped quietly */
    }
    packet_len = session_seal(&client_state.cur, buffer, len, packet,
                              sizeof(packet));
    if (packet_len == 0U) {
        return -1;
    }
    return net_client_send(packet, packet_len);
}

void client_reconnect(void)
{
    net_close();
    memset(&client_recv_state, 0, sizeof(client_recv_state));
    client_wipe();
    client_state.established = 0U;

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

    client_state.last_recv = g_now;
    client_state.last_keepalive = g_now;
    client_reconnect_at = 0;

    if (client_start_handshake() != 0) {
        /* Nothing pending would retry it: start over later */
        log_message(LOG_WARNING, "Failed to send the handshake after "
                    "reconnect; will retry in %ds", CAMEX_RECONNECT_INTERVAL);
        net_close();
        client_reconnect_at = g_now + (time_t)CAMEX_RECONNECT_INTERVAL;
        client_state.last_recv = 0;
    } else {
        client_link_up = 1U;
    }
}

/* Seconds between rekeys: V2_REKEY_AFTER_TIME; the environment variable
 * CAMEX_REKEY_AFTER (5..600) shortens it, for tests */
static double client_rekey_after(void)
{
    static int value = 0;

    if (value == 0) {
        const char *env = getenv("CAMEX_REKEY_AFTER");
        int v = env != NULL ? atoi(env) : 0;

        value = (v >= 5 && v <= V2_REKEY_AFTER_TIME) ? v : V2_REKEY_AFTER_TIME;
    }
    return (double)value;
}

static int client_keepalive_interval(void)
{
    int cap = current_config.server_timeout / 2;

    /* --keepalive 0 turns off frequent probes and loss detection, but the
     * server still needs a sign of life before it expires the client */
    if (current_config.keepalive == 0) {
        return CAMEX_CLIENT_TIMEOUT / 2;
    }
    if (cap < 1) {
        cap = 1;
    }
    return current_config.keepalive < cap ? current_config.keepalive : cap;
}

/* Seconds without an answer after which the link counts as lost: the
 * server timeout, as a keepalive goes at least every half of it; with
 * --keepalive 0 (one every CAMEX_CLIENT_TIMEOUT / 2) a whole interval
 * more (#121) */
static double client_loss_after(void)
{
    double t = (double)current_config.server_timeout;

    if (current_config.keepalive == 0) {
        t += (double)client_keepalive_interval();
    }
    return t;
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

    /* The server answers every keepalive, so a quiet link means the
     * server is gone (#10); with --keepalive 0 too, only later (#121). A
     * handshake that gets no answer counts the same way. */
    if ((client_state.established && client_state.last_recv > 0 &&
         difftime(g_now, client_state.last_recv) >= client_loss_after()) ||
        (!client_state.established && client_state.hs_pending &&
         difftime(g_now, client_state.hs_started) >=
             (double)current_config.server_timeout)) {
        log_message(LOG_ERR,
                    "Connection lost: no answer from server %s:%d for %.0fs",
                    current_config.server_host, current_config.port,
                    client_state.established ? client_loss_after()
                                             : (double)current_config
                                                   .server_timeout);
        client_link_up = 0U;
        client_state.last_recv = 0;
        client_reconnect();
        return;
    }

    /* A rekey that gets no answer while the old session still works
     * (handshakes blocked, say) must not keep the old keys forever */
    if (client_state.established && client_state.hs_pending &&
        difftime(g_now, client_state.hs_started) >= V2_REKEY_TIMEOUT) {
        log_message(LOG_ERR, "Session renewal got no answer from %s:%d "
                    "for %ds — reconnecting", current_config.server_host,
                    current_config.port, V2_REKEY_TIMEOUT);
        client_link_up = 0U;
        client_state.last_recv = 0;
        client_reconnect();
        return;
    }

    /* Handshake retransmission (a new INIT, new ephemeral key) */
    if (client_state.hs_pending &&
        difftime(g_now, client_state.hs_sent) >= V2_HANDSHAKE_RETRY) {
        if (client_send_init() != 0) {
            log_message(LOG_ERR, "Failed to send the handshake to %s:%d — "
                        "forcing reconnect", current_config.server_host,
                        current_config.port);
            net_close();
            client_reconnect_at = 0;
            return;
        }
    }

    /* Rekey: fresh keys every V2_REKEY_AFTER_TIME or V2_REKEY_AFTER_MSGS;
     * the current session keeps working until the new one is up */
    if (client_state.cur.valid && !client_state.hs_pending &&
        (difftime(g_now, client_state.cur.created) >= client_rekey_after() ||
         client_state.cur.send_n >= V2_REKEY_AFTER_MSGS)) {
        (void)client_start_handshake();
    }

    /* Keepalive: an empty DATA packet. The interval is capped at half the
     * server timeout, so an idle client never times out between probes;
     * with --keepalive 0 it is half the server's client timeout. Until the
     * server answers on a new session (its confirmation may be lost), one
     * goes every V2_HANDSHAKE_RETRY seconds: the server drops a session
     * nobody confirmed after V2_PREV_SESSION_GRACE. */
    if (client_state.cur.valid &&
        difftime(g_now, client_state.last_keepalive) >=
            (client_state.confirm_pending
                 ? (double)V2_HANDSHAKE_RETRY
                 : (double)client_keepalive_interval())) {
        uint8_t packet[V2_OVERHEAD];
        size_t packet_len = session_seal(&client_state.cur, NULL, 0U, packet,
                                         sizeof(packet));

        if (packet_len == 0U || net_client_send(packet, packet_len) < 0) {
            log_message(LOG_WARNING,
                        "Failed to send keepalive to %s:%d",
                        current_config.server_host, current_config.port);
        }
        client_state.last_keepalive = g_now;
    }
}

int client_socket_create(const char *host, int port, const char *bind_dev)
{
    int fd;
    (void)bind_dev;  /* bind_dev applied via current_config.bind_dev in net_configure_socket */

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
