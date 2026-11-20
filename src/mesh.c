/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * mesh.c — the client side of mesh networks (mesh.h): the peer socket,
 *          punching, direct paths and the way back through the server
 *
 */

#include "mesh.h"
#include "camex.h"
#include "client.h"
#include "keys.h"
#include "log.h"
#include "monocypher.h"
#include "net.h"
#include "noise.h"
#include "packet.h"
#include "proto.h"
#include "tun.h"
#include "util.h"
#ifdef CAMEX_L2
#include "bridge.h"
#include "l2.h"
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#define close(fd) closesocket(fd)
#endif

typedef enum {
    PEER_ASKED = 0,     /* the server was asked about it */
    PEER_PUNCHING,      /* keys and endpoints: opening the NATs */
    PEER_DIRECT,        /* packets go peer to peer */
    PEER_RELAY,         /* through the server (why says why) */
    PEER_NONE           /* the server has no such mesh peer */
} peer_state_t;

static const char *const state_names[] = {
    "asked", "punching", "direct", "relay", "none"
};

typedef struct {
    uint8_t used;
    char id[CAMEX_CLIENT_ID_LEN];
    uint32_t ip_be;
    uint8_t mac[6];
    uint8_t has_mac;
    uint32_t pair;
    uint8_t dir;             /* the direction of what this client sends */
    uint8_t k_send[32];
    uint8_t k_recv[32];
    uint64_t send_n;
    replay_window_t replay;
    struct sockaddr_in pub;
    struct sockaddr_in lan;
    struct sockaddr_in direct;
    uint8_t has_pub;
    uint8_t has_lan;
    peer_state_t state;
    const char *why;
    time_t asked_at;
    time_t punch_until;
    time_t retry_at;
    time_t last_rx;
    time_t last_tx;
    time_t since;
    long last_punch_ms;
    long punch_start_ms;     /* this round of punches began */
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_mark;
    uint64_t tx_mark;
    uint32_t rx_rate;
    uint32_t tx_rate;
#ifdef CAMEX_L2
    l2_reasm_t *reasm;
#endif
} mesh_peer_t;

static struct {
    int active;
    int fd;
    int l2;
    time_t last_endpoint;
    time_t last_stats;
    time_t last_rate;
    uint32_t net_be;         /* the network's pool, for "is this a peer?" */
    uint32_t mask_be;
    uint32_t gw_be;
    uint32_t own_be;
    mesh_peer_t peers[MESH_PEERS];
} mesh = { 0, -1, 0, 0, 0, 0, 0, 0, 0, 0, { { 0 } } };

static long now_ms(void)
{
#ifndef _WIN32
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (long)(tv.tv_sec % 100000) * 1000L + (long)(tv.tv_usec / 1000);
#else
    return (long)(GetTickCount() % 100000000UL);
#endif
}

void mesh_pair_keys(const uint8_t key[32], int dir, uint8_t out[32])
{
    static const char *const ctx[2] = { "camex p2p dir 0", "camex p2p dir 1" };

    crypto_blake2b_keyed(out, 32U, key, 32U, (const uint8_t *)ctx[dir & 1],
                         strlen(ctx[dir & 1]));
}

static void addr_text(const struct sockaddr_in *a, char *buf, size_t size)
{
    char ip[INET_ADDRSTRLEN];

    inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
    snprintf(buf, size, "%s:%u", ip, (unsigned)ntohs(a->sin_port));
}

static int addr_parse(const char *text, struct sockaddr_in *a)
{
    char ip[INET_ADDRSTRLEN];
    const char *colon = strrchr(text, ':');
    char *end = NULL;
    unsigned long port;

    if (colon == NULL || (size_t)(colon - text) >= sizeof(ip)) {
        return -1;
    }
    memcpy(ip, text, (size_t)(colon - text));
    ip[colon - text] = '\0';
    port = strtoul(colon + 1, &end, 10);
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    if (inet_pton(AF_INET, ip, &a->sin_addr) != 1 || end == colon + 1 ||
        *end != '\0' || port == 0UL || port > 65535UL) {
        return -1;
    }
    a->sin_port = htons((uint16_t)port);
    return 0;
}

int mesh_client_start(int l2)
{
    struct sockaddr_in any;
    char ip[16], mask[16];
    struct in_addr a, m, g;

    mesh_client_stop();
    memset(&mesh, 0, sizeof(mesh));
    mesh.fd = -1;
    mesh.l2 = l2;
    mesh.fd = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (mesh.fd < 0) {
        print_errno_message(LOG_ERR, "socket(peer)");
        return -1;
    }
    memset(&any, 0, sizeof(any));
    any.sin_family = AF_INET;
    if (bind(mesh.fd, (struct sockaddr *)&any, sizeof(any)) != 0 ||
        set_fd_nonblocking(mesh.fd) != 0) {
        print_errno_message(LOG_ERR, "bind(peer)");
        close(mesh.fd);
        mesh.fd = -1;
        return -1;
    }
    if (current_config.local_cidr[0] != '\0' &&
        parse_local_cidr(current_config.local_cidr, ip, sizeof(ip), mask,
                         sizeof(mask)) == 0 &&
        inet_pton(AF_INET, ip, &a) == 1 && inet_pton(AF_INET, mask, &m) == 1) {
        mesh.own_be = a.s_addr;
        mesh.mask_be = m.s_addr;
        mesh.net_be = a.s_addr & m.s_addr;
    }
    if (current_config.gateway_ip[0] != '\0' &&
        inet_pton(AF_INET, current_config.gateway_ip, &g) == 1) {
        mesh.gw_be = g.s_addr;
    }
    mesh.active = 1;
    log_message(LOG_INFO, "Mesh: peers go direct where they can");
    mesh_session_up();
    return 0;
}

void mesh_client_stop(void)
{
    size_t i;

    for (i = 0U; i < MESH_PEERS; ++i) {
        crypto_wipe(mesh.peers[i].k_send, 32U);
        crypto_wipe(mesh.peers[i].k_recv, 32U);
#ifdef CAMEX_L2
        free(mesh.peers[i].reasm);
        mesh.peers[i].reasm = NULL;
#endif
        mesh.peers[i].used = 0U;
    }
    if (mesh.fd >= 0) {
        close(mesh.fd);
    }
    mesh.fd = -1;
    mesh.active = 0;
}

int mesh_client_active(void)
{
    return mesh.active;
}

int mesh_fd(void)
{
    return mesh.active ? mesh.fd : -1;
}

/* ENDPOINT from the peer socket, and the socket's LAN address */
void mesh_session_up(void)
{
    uint8_t pkt[V2_OVERHEAD];
    size_t n;
    struct sockaddr_in server;
    struct sockaddr_in local;
    socklen_t sl = sizeof(local);

    if (!mesh.active || !client_state.cur.valid ||
        net_client_peer_addr(&server) != 0) {
        return;
    }
    n = session_seal_as(&client_state.cur, V2_TYPE_ENDPOINT, NULL, 0U, pkt,
                        sizeof(pkt));
    if (n > 0U) {
        (void)sendto(mesh.fd, (const char *)pkt, (int)n, 0,
                     (struct sockaddr *)&server, sizeof(server));
    }
    mesh.last_endpoint = g_now;
    /* Its address on this host's LAN: the source towards the server, and
     * the peer socket's port */
    if (net_client_local_addr(&local) == 0 &&
        getsockname(mesh.fd, (struct sockaddr *)&server, &sl) == 0) {
        char text[64], msg[96];

        local.sin_port = server.sin_port;
        addr_text(&local, text, sizeof(text));
        snprintf(msg, sizeof(msg), "LAN %s", text);
        (void)client_send_control(msg);
    }
}

static mesh_peer_t *peer_by_ip(uint32_t ip_be)
{
    size_t i;

    for (i = 0U; i < MESH_PEERS; ++i) {
        if (mesh.peers[i].used && mesh.peers[i].ip_be == ip_be &&
            ip_be != 0U) {
            return &mesh.peers[i];
        }
    }
    return NULL;
}

static mesh_peer_t *peer_by_mac(const uint8_t *mac)
{
    size_t i;

    for (i = 0U; i < MESH_PEERS; ++i) {
        if (mesh.peers[i].used && mesh.peers[i].has_mac &&
            memcmp(mesh.peers[i].mac, mac, 6) == 0) {
            return &mesh.peers[i];
        }
    }
    return NULL;
}

static mesh_peer_t *peer_by_pair(uint32_t pair)
{
    size_t i;

    for (i = 0U; i < MESH_PEERS; ++i) {
        if (mesh.peers[i].used && mesh.peers[i].pair == pair && pair != 0U) {
            return &mesh.peers[i];
        }
    }
    return NULL;
}

static mesh_peer_t *peer_by_id(const char *id)
{
    size_t i;

    for (i = 0U; i < MESH_PEERS; ++i) {
        if (mesh.peers[i].used && strcasecmp(mesh.peers[i].id, id) == 0) {
            return &mesh.peers[i];
        }
    }
    return NULL;
}

static mesh_peer_t *peer_new(void)
{
    mesh_peer_t *old = NULL;
    size_t i;

    for (i = 0U; i < MESH_PEERS; ++i) {
        mesh_peer_t *p = &mesh.peers[i];

        if (!p->used) {
            old = p;
            break;
        }
        /* The least used one makes room: a direct path last */
        if (old == NULL ||
            (p->state != PEER_DIRECT && old->state == PEER_DIRECT) ||
            ((p->state == PEER_DIRECT) == (old->state == PEER_DIRECT) &&
             p->last_rx < old->last_rx)) {
            old = p;
        }
    }
#ifdef CAMEX_L2
    free(old->reasm);
#endif
    crypto_wipe(old, sizeof(*old));
    old->used = 1U;
    old->since = g_now;
    return old;
}

static void peer_ask(mesh_peer_t *p)
{
    char msg[64];

    if (p->has_mac && p->ip_be == 0U) {
        char mac[18];

#ifdef CAMEX_L2
        l2_mac_text(p->mac, mac, sizeof(mac));
#else
        snprintf(mac, sizeof(mac), "-");
#endif
        snprintf(msg, sizeof(msg), "PEER? mac=%s", mac);
    } else {
        char ip[INET_ADDRSTRLEN];
        struct in_addr a;

        a.s_addr = p->ip_be;
        inet_ntop(AF_INET, &a, ip, sizeof(ip));
        snprintf(msg, sizeof(msg), "PEER? ip=%s", ip);
    }
    p->asked_at = g_now;
    (void)client_send_control(msg);
}

/* A P2P packet with payload to the peer at a */
static int p2p_send(mesh_peer_t *p, const struct sockaddr_in *a,
                    const uint8_t *payload, size_t len)
{
    uint8_t pkt[V2_P2P_HDR + TUN_PACKET_MAX + NOISE_TAGLEN];

    if (len > TUN_PACKET_MAX || p->send_n >= V2_REJECT_AFTER_MSGS) {
        return -1;
    }
    memcpy(pkt, CAMEX_MAGIC, 4);
    pkt[4] = V2_TYPE_P2P;
    v2_write_u32(pkt + 5, p->pair);
    pkt[9] = p->dir;
    write_le64(pkt + 10, p->send_n);
    noise_encrypt(p->k_send, p->send_n, pkt, V2_P2P_HDR, payload, len,
                  pkt + V2_P2P_HDR);
    p->send_n++;
    p->last_tx = g_now;
    return sendto(mesh.fd, (const char *)pkt,
                  (int)(V2_P2P_HDR + len + NOISE_TAGLEN), 0,
                  (const struct sockaddr *)a, sizeof(*a)) < 0 ? -1 : 0;
}

/* The first punches to the public endpoint carry a low TTL: they open
 * this side's NAT and die on the way, before the peer's NAT. One that got
 * there before the peer sent anything would leave an entry for "inbound
 * from us" in its connection tracking, and the peer's own mapping towards
 * us would then get another port (Linux NAT does that): the punches would
 * miss each other. Then come normal ones, which find both NATs open. */
#define MESH_LOW_TTL 2
#define MESH_LOW_TTL_FOR_MS 1000

static void punch(mesh_peer_t *p)
{
    static const uint8_t ping[2] = { MESH_PUNCH, 'P' };

    if (p->has_pub) {
#ifdef IP_TTL
        long since = now_ms() - p->punch_start_ms;
        int low = since >= 0 && since < MESH_LOW_TTL_FOR_MS;
        int ttl = MESH_LOW_TTL;
        int def = 64;
        socklen_t dl = sizeof(def);

        if (low) {
            (void)getsockopt(mesh.fd, IPPROTO_IP, IP_TTL, (char *)&def, &dl);
            (void)setsockopt(mesh.fd, IPPROTO_IP, IP_TTL, (const char *)&ttl,
                             sizeof(ttl));
        }
        (void)p2p_send(p, &p->pub, ping, sizeof(ping));
        if (low) {
            (void)setsockopt(mesh.fd, IPPROTO_IP, IP_TTL, (const char *)&def,
                             sizeof(def));
        }
#else
        (void)p2p_send(p, &p->pub, ping, sizeof(ping));
#endif
    }
    if (p->has_lan) {
        (void)p2p_send(p, &p->lan, ping, sizeof(ping));
    }
    p->last_punch_ms = now_ms();
}

static void peer_to_relay(mesh_peer_t *p, const char *why, int retry_in)
{
    if (p->state == PEER_DIRECT) {
        log_message(LOG_NOTICE, "Mesh: peer %s back through the server: %s",
                    p->id, why);
    }
    p->state = PEER_RELAY;
    p->why = why;
    p->retry_at = g_now + retry_in;
}

#ifdef CAMEX_L2
static mesh_peer_t *l2_target;

static int l2_piece(void *ctx, const uint8_t *payload, size_t len)
{
    (void)ctx;
    return p2p_send(l2_target, &l2_target->direct, payload, len);
}
#endif

int mesh_send(uint32_t dst_ip_be, const uint8_t *dst_mac,
              const uint8_t *payload, size_t len)
{
    mesh_peer_t *p;

    if (!mesh.active) {
        return 0;
    }
    if (dst_mac != NULL) {
        if ((dst_mac[0] & 1U) != 0U) {
            return 0;   /* broadcast, multicast: the server floods them */
        }
        p = peer_by_mac(dst_mac);
    } else {
        /* Only the network's other clients are peers */
        if (mesh.mask_be == 0U ||
            (dst_ip_be & mesh.mask_be) != mesh.net_be ||
            dst_ip_be == mesh.gw_be || dst_ip_be == mesh.own_be) {
            return 0;
        }
        p = peer_by_ip(dst_ip_be);
    }
    if (p == NULL) {
        p = peer_new();
        if (dst_mac != NULL) {
            memcpy(p->mac, dst_mac, 6);
            p->has_mac = 1U;
        } else {
            p->ip_be = dst_ip_be;
        }
        p->state = PEER_ASKED;
        peer_ask(p);
        return 0;
    }
    if (p->state == PEER_DIRECT) {
        int rc;

#ifdef CAMEX_L2
        if (mesh.l2) {
            l2_target = p;
            rc = l2_send_frame(payload, len, l2_piece, NULL);
        } else
#endif
        {
            rc = p2p_send(p, &p->direct, payload, len);
        }
        if (rc == 0) {
            p->tx_bytes += len;
            return 1;
        }
        return -1;
    }
    /* Asked again now and then: a peer may come, a NAT may change */
    if ((p->state == PEER_RELAY || p->state == PEER_NONE ||
         p->state == PEER_ASKED) &&
        difftime(g_now, p->retry_at) >= 0.0 &&
        difftime(g_now, p->asked_at) >= (double)MESH_ASK_AGAIN) {
        peer_ask(p);
    }
    return 0;
}

/* A P2P payload that is tunnel data */
static void p2p_data(mesh_peer_t *p, const uint8_t *plain, size_t len)
{
#ifdef CAMEX_L2
    if (mesh.l2) {
        const uint8_t *frame = NULL;
        long flen;

        if (p->reasm == NULL) {
            p->reasm = calloc(1U, sizeof(*p->reasm));
        }
        flen = l2_receive(p->reasm, plain, len, &frame, g_now);
        if (flen <= 0) {
            return;
        }
        p->rx_bytes += (uint64_t)flen;
        if (bridge_active()) {
            (void)bridge_from_tunnel(frame, (size_t)flen);
        } else {
            (void)packet_write(frame, (size_t)flen);
        }
        return;
    }
#endif
    {
        uint32_t src;
        uint32_t dst;

        /* A peer sends only from its own tunnel address, to ours */
        if (ipv4_parse_endpoints(plain, len, &src, &dst) != 0 ||
            src != p->ip_be || dst != mesh.own_be) {
            return;
        }
        p->rx_bytes += len;
        (void)packet_write(plain, len);
    }
}

void mesh_readable(void)
{
    uint8_t pkt[V2_P2P_HDR + TUN_PACKET_MAX + NOISE_TAGLEN];
    uint8_t plain[TUN_PACKET_MAX];
    unsigned int budget = 256U;

    while (mesh.active && budget-- > 0U) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = (int)recvfrom(mesh.fd, (char *)pkt, (int)sizeof(pkt), 0,
                              (struct sockaddr *)&from, &fl);
        mesh_peer_t *p;
        uint64_t ctr;
        size_t plen;

        if (n <= 0) {
            break;
        }
        if (v2_packet_type(pkt, (size_t)n) != V2_TYPE_P2P) {
            continue;
        }
        p = peer_by_pair(v2_read_u32(pkt + 5));
        if (p == NULL || pkt[9] == p->dir) {
            continue;
        }
        ctr = read_le64(pkt + 10);
        plen = (size_t)n - V2_P2P_HDR - NOISE_TAGLEN;
        if (replay_check(&p->replay, ctr) != 0 ||
            noise_decrypt(p->k_recv, ctr, pkt, V2_P2P_HDR, pkt + V2_P2P_HDR,
                          (size_t)n - V2_P2P_HDR, plain) != 0) {
            continue;
        }
        replay_record(&p->replay, ctr);
        p->last_rx = g_now;
        if (plen >= 2U && plain[0] == MESH_PUNCH) {
            if (plain[1] == 'P') {
                static const uint8_t ack[2] = { MESH_PUNCH, 'A' };

                (void)p2p_send(p, &from, ack, sizeof(ack));
            }
            if ((plain[1] == 'P' || plain[1] == 'A') &&
                p->state != PEER_DIRECT) {
                char where[32];

                p->direct = from;
                p->state = PEER_DIRECT;
                p->why = "";
                p->since = g_now;
                addr_text(&from, where, sizeof(where));
                log_message(LOG_NOTICE, "Mesh: peer %s direct at %s", p->id,
                            where);
            }
            continue;
        }
        /* Data takes the path it came on as the peer's (a NAT rebind) */
        if (p->state == PEER_DIRECT) {
            p->direct = from;
        }
        p2p_data(p, plain, plen);
    }
}

/* key=value of a control message */
static int field(const char *msg, const char *key, char *out, size_t size)
{
    size_t kl = strlen(key);
    const char *p = msg;

    while ((p = strstr(p, key)) != NULL) {
        if ((p == msg || p[-1] == ' ') && p[kl] == '=') {
            const char *v = p + kl + 1;
            size_t n = strcspn(v, " \n");

            if (n >= size) {
                return -1;
            }
            memcpy(out, v, n);
            out[n] = '\0';
            return 0;
        }
        p += kl;
    }
    return -1;
}

void mesh_control(const uint8_t *data, size_t len)
{
    char msg[512];
    char id[CAMEX_CLIENT_ID_LEN], v[64], key64[64], pair[16];
    mesh_peer_t *p = NULL;
    uint8_t key[32];
    struct in_addr a;
    uint8_t mac[6];
    int have_ip = 0, have_mac = 0;

    if (!mesh.active || len == 0U || len >= sizeof(msg)) {
        return;
    }
    memcpy(msg, data, len);
    msg[len] = '\0';
    if (field(msg, "ip", v, sizeof(v)) == 0 &&
        inet_pton(AF_INET, v, &a) == 1) {
        have_ip = 1;
    }
#ifdef CAMEX_L2
    if (field(msg, "mac", v, sizeof(v)) == 0 && l2_mac_parse(v, mac) == 0) {
        have_mac = 1;
    }
#endif
    if (strncmp(msg, "NOPEER ", 7) == 0) {
        p = have_ip ? peer_by_ip(a.s_addr) : have_mac ? peer_by_mac(mac) : NULL;
        if (p != NULL && p->state != PEER_DIRECT) {
            p->state = PEER_NONE;
            p->why = "the server has no mesh peer there";
            p->retry_at = g_now + MESH_ASK_AGAIN;
        }
        return;
    }
    if (strncmp(msg, "PEER ", 5) != 0 ||
        field(msg, "id", id, sizeof(id)) != 0 ||
        field(msg, "key", key64, sizeof(key64)) != 0 ||
        key_parse_b64(key64, key) != 0 ||
        field(msg, "pair", pair, sizeof(pair)) != 0) {
        return;
    }
    p = peer_by_id(id);
    if (p == NULL && have_ip) {
        p = peer_by_ip(a.s_addr);
    }
    if (p == NULL && have_mac) {
        p = peer_by_mac(mac);
    }
    if (p == NULL) {
        p = peer_new();
    }
    snprintf(p->id, sizeof(p->id), "%s", id);
    if (have_ip) {
        p->ip_be = a.s_addr;
    }
    if (have_mac) {
        memcpy(p->mac, mac, 6);
        p->has_mac = 1U;
    }
    p->pair = (uint32_t)strtoul(pair, NULL, 16);
    p->dir = strcasecmp(current_config.client_id, id) < 0 ? 0U : 1U;
    mesh_pair_keys(key, p->dir, p->k_send);
    mesh_pair_keys(key, 1 - p->dir, p->k_recv);
    crypto_wipe(key, sizeof(key));
    p->send_n = 0U;
    memset(&p->replay, 0, sizeof(p->replay));
    p->has_pub = field(msg, "pub", v, sizeof(v)) == 0 &&
                 addr_parse(v, &p->pub) == 0;
    p->has_lan = field(msg, "lan", v, sizeof(v)) == 0 &&
                 addr_parse(v, &p->lan) == 0;
    if (!p->has_pub && !p->has_lan) {
        peer_to_relay(p, "the peer has no endpoint yet", MESH_RETRY);
        return;
    }
    /* Both sides got this at once: both punch now */
    if (p->state != PEER_DIRECT) {
        p->state = PEER_PUNCHING;
        p->why = "";
    }
    p->punch_until = g_now + MESH_PUNCH_FOR;
    p->punch_start_ms = now_ms();
    punch(p);
}

void mesh_tick(void)
{
    size_t i;
    long ms = now_ms();
    int rates = 0;

    if (!mesh.active) {
        return;
    }
    if (difftime(g_now, mesh.last_rate) >= 1.0) {
        rates = 1;
        mesh.last_rate = g_now;
    }
    /* The NAT keeps the peer socket's mapping while it is used */
    if (difftime(g_now, mesh.last_endpoint) >= 20.0) {
        mesh_session_up();
    }
    for (i = 0U; i < MESH_PEERS; ++i) {
        mesh_peer_t *p = &mesh.peers[i];

        if (!p->used) {
            continue;
        }
        if (rates) {
            p->rx_rate = (uint32_t)(p->rx_bytes - p->rx_mark);
            p->tx_rate = (uint32_t)(p->tx_bytes - p->tx_mark);
            p->rx_mark = p->rx_bytes;
            p->tx_mark = p->tx_bytes;
        }
        switch (p->state) {
        case PEER_PUNCHING:
            if (difftime(g_now, p->punch_until) > 0.0) {
                peer_to_relay(p, "no answer to punches (NAT)", MESH_RETRY);
                log_message(LOG_NOTICE, "Mesh: peer %s stays on the server: "
                            "no answer to punches", p->id);
            } else if (ms - p->last_punch_ms >= MESH_PUNCH_EVERY_MS ||
                       ms < p->last_punch_ms) {
                punch(p);
            }
            break;
        case PEER_DIRECT:
            if (difftime(g_now, p->last_rx) >= (double)MESH_DEAD) {
                peer_to_relay(p, "the direct path went silent", 5);
            } else if (difftime(g_now, p->last_tx) >=
                       (double)MESH_KEEPALIVE) {
                static const uint8_t ka[2] = { MESH_PUNCH, 'K' };

                (void)p2p_send(p, &p->direct, ka, sizeof(ka));
            }
            break;
        default:
            break;
        }
    }
    /* Each peer's state for the server's --status and --top */
    if (difftime(g_now, mesh.last_stats) >= (double)MESH_STATS_EVERY) {
        char msg[CAMEX_CONTROL_MAX - 32];
        size_t off;

        mesh.last_stats = g_now;
        off = (size_t)snprintf(msg, sizeof(msg), "STATS");
        for (i = 0U; i < MESH_PEERS && off < sizeof(msg) - 96U; ++i) {
            const mesh_peer_t *p = &mesh.peers[i];

            if (!p->used || p->id[0] == '\0') {
                continue;
            }
            off += (size_t)snprintf(msg + off, sizeof(msg) - off,
                                    "|%s %s %llu %llu %u %u %ld", p->id,
                                    state_names[p->state],
                                    (unsigned long long)p->rx_bytes,
                                    (unsigned long long)p->tx_bytes,
                                    (unsigned)p->rx_rate,
                                    (unsigned)p->tx_rate,
                                    (long)difftime(g_now, p->since));
        }
        (void)client_send_control(msg);
    }
}

size_t mesh_describe(char *buf, size_t size)
{
    size_t off = 0U;
    size_t i;

    if (size == 0U) {
        return 0U;
    }
    buf[0] = '\0';
    for (i = 0U; i < MESH_PEERS && off + 1U < size; ++i) {
        const mesh_peer_t *p = &mesh.peers[i];

        if (!p->used || p->id[0] == '\0') {
            continue;
        }
        off += (size_t)snprintf(buf + off, size - off, "%s %s %llu %llu%s%s\n",
                                p->id, state_names[p->state],
                                (unsigned long long)p->rx_bytes,
                                (unsigned long long)p->tx_bytes,
                                p->why != NULL && p->why[0] != '\0' ? " " : "",
                                p->why != NULL ? p->why : "");
    }
    return off;
}
