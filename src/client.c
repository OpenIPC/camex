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
#include "tcpgate.h"
#include "keys.h"
#include "netw.h"
#include "pake.h"
#include "monocypher.h"
#ifdef CAMEX_MESH
#include "mesh.h"
#endif
#ifdef CAMEX_L2
#include "l2.h"
#include "bridge.h"

static l2_reasm_t *client_reasm = NULL;   /* the frame in assembly */
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <time.h>

#ifndef _WIN32
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/file.h>
#include <fcntl.h>
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
    /* A new run (camex_main again in one process, the Android library)
     * starts with the shortest reconnect delay */
    reconnect_backoff = 0;
}

void client_wipe(void)
{
    noise_wipe(&client_state.hs);
    session_wipe(&client_state.cur);
    session_wipe(&client_state.prev);
    client_state.hs_pending = 0U;
}

/* "<host>:<port>" of the server, the key of the known servers file */
static void client_known_name(char *buf, size_t size)
{
    snprintf(buf, size, "%s:%d", current_config.server_host,
             current_config.port);
}

static const char *client_known_path(void)
{
    if (strcmp(current_config.known_servers, "none") == 0) {
        return NULL;
    }
    return current_config.known_servers[0] != '\0'
               ? current_config.known_servers : CAMEX_KNOWN_SERVERS_PATH;
}

/* 1 while the key in use came from the known servers file: a server whose
 * key changed is then told how to forget the old one */
static uint8_t client_key_from_file = 0U;

/* This server's line of the known servers file: its key, and whether a
 * CPace with it ever succeeded ("pake" after the key). 0 or -1 */
static int client_known_find(char *key, size_t key_size, int *pake_mark)
{
    const char *path = client_known_path();
    char name[sizeof(current_config.server_host) + 8];
    char line[256];
    FILE *fp;
    int found = -1;

    *pake_mark = 0;
    if (path == NULL || (fp = fopen(path, "r")) == NULL) {
        return -1;
    }
    client_known_name(name, sizeof(name));
    while (fgets(line, (int)sizeof(line), fp) != NULL) {
        char *sp;
        char *mark;

        trim_whitespace(line);
        sp = strchr(line, ' ');
        if (line[0] == '#' || sp == NULL) {
            continue;
        }
        *sp = '\0';
        if (strcasecmp(line, name) != 0) {
            continue;
        }
        trim_whitespace(sp + 1);
        mark = strchr(sp + 1, ' ');
        if (mark != NULL) {
            *mark++ = '\0';
            trim_whitespace(mark);
            *pake_mark = strcmp(mark, "pake") == 0;
        }
        snprintf(key, key_size, "%s", sp + 1);
        found = 0;
        break;
    }
    fclose(fp);
    return found;
}

int client_known_load(void)
{
    char key[64];
    int mark;

    if (client_known_find(key, sizeof(key), &mark) != 0 ||
        crypto_set_server_pub(key) != 0) {
        return -1;
    }
    client_key_from_file = 1U;
    log_message(LOG_INFO, "Server key of %s:%d from %s",
                current_config.server_host, current_config.port,
                client_known_path());
    return 0;
}

/* Keep the key the server gave on first contact: the file is rewritten
 * through a new one, so a power cut leaves the old or the new file */
static void client_known_store(const char *pub_b64, int pake_mark)
{
    const char *path = client_known_path();
    char name[sizeof(current_config.server_host) + 8];
    char tmp[sizeof(current_config.known_servers) + 8];
    char line[256];
    FILE *in;
    FILE *out;
#ifndef _WIN32
    int lockfd;
#endif

    if (path == NULL) {
        return;
    }
    client_known_name(name, sizeof(name));
#ifndef _WIN32
    /* Two clients of one host share the file: one at a time reads it and
     * puts it back (<path>.lock), each through a file of its own, never
     * a fixed name another one, or a planted link, could be at */
    snprintf(tmp, sizeof(tmp), "%s.lock", path);
    lockfd = open(tmp, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    {
        int tries, locked = 0;

        for (tries = 0; lockfd >= 0 && tries < 40; ++tries) {
            if (flock(lockfd, LOCK_EX | LOCK_NB) == 0) {
                locked = 1;
                break;
            }
            usleep(50000);
        }
        /* Without the lock, a rewrite could drop another writer's line
         * (its key, its CPace mark): better not to keep this one now */
        if (!locked) {
            log_message(LOG_WARNING, "Cannot keep the server key in %s "
                        "(%s busy or not writable): it is asked for again at "
                        "the next start", path, tmp);
            if (lockfd >= 0) {
                close(lockfd);
            }
            return;
        }
    }
    snprintf(tmp, sizeof(tmp), "%s.XXXXXX", path);
    {
        int fd = mkstemp(tmp);

        out = fd >= 0 ? fdopen(fd, "w") : NULL;
        if (fd >= 0 && out == NULL) {
            close(fd);
            (void)remove(tmp);
        }
    }
#else
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    out = fopen(tmp, "w");
#endif
    if (out == NULL) {
        log_message(LOG_WARNING, "Cannot keep the server key in %s (%s): "
                    "it is asked for again at the next start", path,
                    strerror(errno));
#ifndef _WIN32
        if (lockfd >= 0) {
            close(lockfd);
        }
#endif
        return;
    }
    in = fopen(path, "r");
    if (in != NULL) {
        while (fgets(line, (int)sizeof(line), in) != NULL) {
            char copy[256];
            char *sp;

            snprintf(copy, sizeof(copy), "%s", line);
            trim_whitespace(copy);
            sp = strchr(copy, ' ');
            if (sp != NULL) {
                *sp = '\0';
                if (strcasecmp(copy, name) == 0) {
                    continue;   /* replaced below */
                }
            }
            fputs(line, out);
        }
        fclose(in);
    }
    fprintf(out, "%s %s%s\n", name, pub_b64, pake_mark ? " pake" : "");
    if (fclose(out) != 0 || rename(tmp, path) != 0) {
        log_message(LOG_WARNING, "Cannot keep the server key in %s (%s)",
                    path, strerror(errno));
        (void)remove(tmp);
    } else {
        log_message(LOG_DEBUG, "Server key of %s kept in %s", name, path);
    }
#ifndef _WIN32
    if (lockfd >= 0) {
        close(lockfd);   /* and its lock */
    }
#endif
}

/* First contact without the server's key: ask for it. The request is
 * longer than the answer (V2_KEYREQ_LEN) */
static int client_send_keyreq(void)
{
    uint8_t packet[V2_KEYREQ_LEN];

    memset(packet, 0, sizeof(packet));
    memcpy(packet, CAMEX_MAGIC, 4);
    packet[4] = V2_TYPE_KEYREQ;
    client_state.hs_pending = 1U;
    client_state.hs_sent = g_now;
    client_state.key_requested = 1U;
    return net_client_send(packet, sizeof(packet)) < 0 ? -1 : 0;
}

static int client_send_init(void);
static int client_send_init_noise(void);
static void client_pake_new_connection(void);

/* The server's key on first contact: kept (trust on first use), then the
 * handshake starts */
static int client_handle_key(const uint8_t *buffer, size_t len)
{
    char b64[KEY_B64_LEN + 1U];

    if (crypto_get_ctx()->has_server_pub || !client_state.key_requested ||
        len != V2_KEY_LEN) {
        return -1;
    }
    if (key_base64_encode(buffer + 5, KEY_LEN, b64, sizeof(b64)) == 0U ||
        crypto_set_server_pub(b64) != 0) {
        return -1;
    }
    client_state.key_requested = 0U;
    /* With CPace the key is checked by the network's name first */
    if (current_config.pake) {
        return client_send_init();
    }
    log_message(LOG_NOTICE, "First contact with server %s:%d: its key %s "
                "is trusted from now on", current_config.server_host,
                current_config.port, b64);
    client_known_store(b64, 0);
    return client_send_init();
}

/*
 * CPace (pake.h). Each connection: the server's key (KEYREQ), PAKE1, the
 * server's PAKE2 with one answer per network it knows; the one whose tag
 * opens is ours, PAKE3 confirms it, and its key is the handshake's PSK.
 * The network's ID is never sent. A server that does not know the network
 * but takes new ones (or one that does not answer PAKE1, an older camex)
 * gets the ID the old way, but only while this client never completed a
 * CPace with it: then a server that claims to be open is an impostor.
 */
static struct {
    uint8_t sid[PAKE_SID_LEN];
    uint8_t secret[32];
    uint8_t ya[32];
    uint8_t sent;          /* PAKE1 out, no answer yet */
    unsigned int unanswered;
    uint8_t fallback;      /* the old way for this connection */
    uint8_t marked;        /* the known servers file says "pake" */
} cpace;

void client_pake_load_mark(void)
{
    char key[64];
    int mark = 0;

    cpace.marked = client_known_find(key, sizeof(key), &mark) == 0 && mark;
    crypto_wipe(key, sizeof(key));
}

/* A new connection: the server's key asked for again (a new one is
 * proven by CPace too), a new CPace */
static void client_pake_new_connection(void)
{
    if (current_config.pake) {
        if (!current_config.pinned) {
            crypto_clear_server_pub();
        }
        client_pake_reset();
    }
}

void client_pake_reset(void)
{
    crypto_wipe(cpace.secret, sizeof(cpace.secret));
    cpace.sent = 0U;
    cpace.unanswered = 0U;
    cpace.fallback = 0U;
    current_config.pake_ok = 0U;
    current_config.send_net_name = 0U;
    if (current_config.pake) {
        uint8_t k[32];

        /* The fixed network key again, for a fallback */
        netw_handshake_key(k);
        crypto_set_psk(k);
        crypto_wipe(k, sizeof(k));
    }
}

/* The ID the old way: allowed only for a server never proven by CPace */
static int client_pake_fall_back(const char *why)
{
    /* A pinned key: only the real server can read the ID */
    if (current_config.pinned) {
        cpace.fallback = 1U;
        return 0;
    }
    if (cpace.marked) {
        log_message(LOG_ERR, "Server %s:%d %s, but it proved it knows this "
                    "network before: not trusted (an impostor?)",
                    current_config.server_host, current_config.port, why);
        return -1;
    }
    log_message(LOG_WARNING, "Server %s:%d %s: joining by the network's ID, "
                "its key trusted on first use", current_config.server_host,
                current_config.port, why);
    cpace.fallback = 1U;
    {
        char b64[KEY_B64_LEN + 1U];

        key_base64_encode(crypto_get_ctx()->server_pub, KEY_LEN, b64,
                          sizeof(b64));
        client_known_store(b64, 0);
    }
    return 0;
}

static int client_send_pake1(void)
{
    uint8_t pkt[V2_PAKE1_LEN];
    uint8_t g[32];
    uint8_t id[CAMEX_NET_ID_LEN];

    if (get_random_bytes(cpace.sid, sizeof(cpace.sid)) != 0 ||
        get_random_bytes(cpace.secret, sizeof(cpace.secret)) != 0) {
        return -1;
    }
    netw_id_from_name(current_config.network, id);
    pake_generator(id, cpace.sid, g);
    crypto_wipe(id, sizeof(id));
    if (pake_mult(cpace.ya, cpace.secret, g) != 0) {
        return -1;
    }
    memset(pkt, 0, sizeof(pkt));
    memcpy(pkt, CAMEX_MAGIC, 4);
    pkt[4] = V2_TYPE_PAKE1;
    memcpy(pkt + 5, cpace.sid, PAKE_SID_LEN);
    memcpy(pkt + 5 + PAKE_SID_LEN, cpace.ya, 32U);
    cpace.sent = 1U;
    client_state.hs_pending = 1U;
    client_state.hs_sent = g_now;
    return net_client_send(pkt, sizeof(pkt)) < 0 ? -1 : 0;
}

static int client_handle_pake2(const uint8_t *buf, size_t len)
{
    const camex_crypto_t *ctx = crypto_get_ctx();
    unsigned int flags;
    unsigned int count;
    unsigned int i;
    uint8_t k[32];
    uint8_t tag[PAKE_TAG_LEN];

    if (!current_config.pake || !cpace.sent || current_config.pake_ok ||
        memcmp(buf + 5, cpace.sid, PAKE_SID_LEN) != 0) {
        return -1;
    }
    flags = buf[5 + PAKE_SID_LEN];
    count = buf[5 + PAKE_SID_LEN + 1U];
    if (count > PAKE_MAX_NETS ||
        len != 5U + PAKE_SID_LEN + 2U + count * PAKE_ENTRY_LEN) {
        return -1;
    }
    cpace.sent = 0U;
    cpace.unanswered = 0U;
    for (i = 0U; i < count; ++i) {
        const uint8_t *e = buf + 5 + PAKE_SID_LEN + 2U + i * PAKE_ENTRY_LEN;

        if (pake_mult(k, cpace.secret, e) != 0) {
            continue;
        }
        pake_tag(tag, 's', k, cpace.sid, cpace.ya, e, ctx->server_pub);
        if (crypto_verify16(tag, e + 32) == 0) {
            uint8_t p3[V2_PAKE3_LEN];
            uint8_t psk[32];
            char b64[KEY_B64_LEN + 1U];
            char kept[64];
            int mark = 0;

            memcpy(p3, CAMEX_MAGIC, 4);
            p3[4] = V2_TYPE_PAKE3;
            memcpy(p3 + 5, cpace.sid, PAKE_SID_LEN);
            p3[5 + PAKE_SID_LEN] = (uint8_t)i;
            pake_tag(p3 + 5 + PAKE_SID_LEN + 1U, 'c', k, cpace.sid, cpace.ya,
                     e, ctx->server_pub);
            pake_psk(psk, k);
            crypto_wipe(k, sizeof(k));
            crypto_wipe(cpace.secret, sizeof(cpace.secret));
            crypto_set_psk(psk);
            crypto_wipe(psk, sizeof(psk));
            current_config.pake_ok = 1U;
            current_config.send_net_name = current_config.pinned &&
                (flags & PAKE_FLAG_NAME) != 0U ? 1U : 0U;
            (void)net_client_send(p3, sizeof(p3));
            if (current_config.pinned) {
                return client_send_init();
            }
            /* The key, proven by the name, kept with the mark */
            key_base64_encode(ctx->server_pub, KEY_LEN, b64, sizeof(b64));
            if (client_known_find(kept, sizeof(kept), &mark) != 0 ||
                strcmp(kept, b64) != 0 || !mark) {
                log_message(LOG_NOTICE, "Server %s:%d proved it knows the "
                            "network (CPace); its key %s is kept",
                            current_config.server_host, current_config.port,
                            b64);
                client_known_store(b64, 1);
            }
            cpace.marked = 1U;
            return client_send_init();
        }
    }
    crypto_wipe(k, sizeof(k));
    if ((flags & PAKE_FLAG_OPEN) != 0U) {
        if (client_pake_fall_back("does not know the network but takes new "
                                  "ones") == 0) {
            /* The name only to a pinned key: a key believed on first
             * use may be an impostor's */
            current_config.send_net_name = current_config.pinned &&
                (flags & PAKE_FLAG_NAME) != 0U ? 1U : 0U;
            return client_send_init();
        }
        return -1;
    }
    log_message(LOG_ERR, "Server %s:%d does not know this network (or is not "
                "the real server)", current_config.server_host,
                current_config.port);
    return -1;
}

static int client_send_init(void)
{
    const camex_crypto_t *ctx = crypto_get_ctx();

    /* CPace first (an older server never answers PAKE1: after three tries
     * it gets the ID the old way, if allowed) */
    if (current_config.pake && !current_config.pake_ok && !cpace.fallback) {
        if (!ctx->has_server_pub) {
            return client_send_keyreq();
        }
        if (cpace.sent && ++cpace.unanswered >= 3U) {
            cpace.sent = 0U;
            if (client_pake_fall_back("does not answer CPace (an older "
                                      "camex?)") != 0) {
                return 0;
            }
        } else {
            return client_send_pake1();
        }
    }
    return client_send_init_noise();
}

static int client_send_init_noise(void)
{
    const camex_crypto_t *ctx = crypto_get_ctx();
    uint8_t payload[V2_TIMESTAMP_LEN + CAMEX_CONTROL_MAX];
    uint8_t packet[V2_INIT_HDR + NOISE_MSG1_OVERHEAD + sizeof(payload) +
                   V2_MAC_LEN];
    uint8_t e_seed[NOISE_DHLEN];
    char message[CAMEX_CONTROL_MAX];
    size_t msg_len;
    size_t len;

    if (!ctx->has_global) {
        log_message(LOG_ERR, "Missing the PSK");
        return -1;
    }
    if (!ctx->has_server_pub) {
        return client_send_keyreq();
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

#ifdef CAMEX_L2
    if (bridge_active()) {
        return;   /* a bridge has no tunnel interface to change */
    }
#endif

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
        probe.l2 != current_config.l2 ||
        memcmp(probe.l2_mac, current_config.l2_mac, 6) != 0 ||
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
    /* atomic: the Android library reads it from another thread */
    __atomic_store_n(&client_state.established, 1U, __ATOMIC_RELEASE);
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
        log_message(LOG_NOTICE, "Connection to server %s:%d established (%s)",
                    current_config.server_host, current_config.port,
                    current_config.transport == CAMEX_TRANSPORT_TCP ? "TCP"
                                                                    : "UDP");
    } else {
        log_message(LOG_INFO, "Session keys renewed");
    }
#ifdef CAMEX_MESH
    mesh_session_up();   /* the server learns the peer socket's address */
#endif
    return 0;
}

int client_handle_net_packet(const uint8_t *buffer, size_t len)
{
    uint8_t plain[TUN_PACKET_MAX];
    size_t plain_len = 0U;
    session_t *s = NULL;
    uint32_t idx;

    switch (v2_packet_type(buffer, len)) {
    case V2_TYPE_KEY:
        return client_handle_key(buffer, len);
    case V2_TYPE_PAKE2:
        return client_handle_pake2(buffer, len);
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
    /* A control message of the server (mesh.h) */
    if (plain[0] == 0xC0U) {
#ifdef CAMEX_MESH
        mesh_control(plain + 1, plain_len - 1U);
#endif
        return 0;
    }
#ifdef CAMEX_L2
    if (current_config.l2) {
        const uint8_t *frame = NULL;
        long flen;

        if (client_reasm == NULL) {
            client_reasm = calloc(1U, sizeof(*client_reasm));
        }
        flen = l2_receive(client_reasm, plain, plain_len, &frame, g_now);
        if (flen < 0) {
            log_peer(LOG_WARNING, "Dropped a malformed frame from the "
                     "server");
            return -1;
        }
        if (flen == 0) {
            return 0;
        }
        /* --bridge: onto the LAN (or to the host), no TAP device */
        if (bridge_active()) {
            return bridge_from_tunnel(frame, (size_t)flen);
        }
        return packet_write(frame, (size_t)flen);
    }
#endif
    if (tcpgate_active()) {
        uint8_t rst[TCPGATE_RST_LEN];
        size_t rst_len;

        /* A connection the app does not take (#187): refused at once */
        if (!tcpgate_check(plain, plain_len, rst, sizeof(rst), &rst_len)) {
            if (rst_len != 0U) {
                (void)client_handle_tun_packet(rst, rst_len);
            }
            return 0;
        }
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
        client_pake_new_connection();
        for (step = 0U; step < (unsigned int)CAMEX_RECONNECT_INTERVAL &&
                        running; ++step) {
            camex_pause_1s();
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

    log_message(LOG_DEBUG, "Waiting for auto-config from server...");
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
            /* A server that drops every handshake may have a new key: the
             * kept one is never replaced on its own. Said after the second
             * silent round, then with the notes above */
            if (client_key_from_file &&
                (renewed == 2U || (renewed > 2U && renewed % 12U == 1U))) {
                log_message(LOG_WARNING, "The server's key came from %s: "
                            "if the server got a new key, remove its line "
                            "there (or give --server-pubkey)",
                            client_known_path());
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
                /* A server whose pool is full may close it for long:
                 * said once, then every 12th time */
                if (renewed++ % 12U == 0U) {
                    log_message(LOG_WARNING, "Connection to the server "
                                "closed before its config; reconnecting "
                                "(and so every %ds while it does)",
                                CAMEX_RECONNECT_INTERVAL);
                }
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
                ((current_config.local_cidr[0] != '\0' &&
                  current_config.gateway_ip[0] != '\0') ||
                 current_config.l2)) {
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
        if (current_config.route_count > 0U) {
            log_message(LOG_INFO, "Received config from server: %s, gateway "
                        "%s, MTU %d, %u route(s)", current_config.local_cidr,
                        current_config.gateway_ip, current_config.mtu,
                        (unsigned)current_config.route_count);
        } else {
            log_message(LOG_INFO, "Received config from server: %s, gateway "
                        "%s, MTU %d", current_config.local_cidr,
                        current_config.gateway_ip, current_config.mtu);
        }
    }

    return rc;
}

#ifdef CAMEX_L2
/* A TAP network: frames in pieces (l2.h), each in its own DATA packet */
static int client_l2_send(void *ctx, const uint8_t *payload, size_t len)
{
    uint8_t packet[1U + L2_FRAG_HDR + L2_PIECE + V2_OVERHEAD];
    size_t packet_len;

    (void)ctx;
    packet_len = session_seal(&client_state.cur, payload, len, packet,
                              sizeof(packet));
    if (packet_len == 0U) {
        return -1;
    }
    return net_client_send(packet, packet_len) < 0 ? -1 : 0;
}
#endif

int client_send_control(const char *text)
{
    uint8_t plain[CAMEX_CONTROL_MAX];
    uint8_t packet[CAMEX_CONTROL_MAX + V2_OVERHEAD];
    size_t n = strlen(text);
    size_t len;

    if (!client_state.cur.valid || n + 1U > sizeof(plain)) {
        return -1;
    }
    plain[0] = 0xC0U;   /* MESH_CTRL */
    memcpy(plain + 1, text, n);
    len = session_seal(&client_state.cur, plain, n + 1U, packet,
                       sizeof(packet));
    return len == 0U || net_client_send(packet, len) < 0 ? -1 : 0;
}

int client_handle_tun_packet(const uint8_t *buffer, size_t len)
{
    uint8_t packet[TUN_PACKET_MAX + V2_OVERHEAD];
    size_t packet_len;

    if (buffer == NULL || len == 0U || len > TUN_PACKET_MAX) {
        return -1;
    }
#ifdef CAMEX_MESH
    /* A mesh peer with a direct path takes it, with or without a session
     * with the server: its pair keys and its path are the peers' own, so a
     * server that is gone (or being restarted) does not cut them off */
    if (mesh_client_active()) {
        int rc;

        if (current_config.l2) {
            rc = len >= 14U ? mesh_send(0U, buffer, buffer, len) : 0;
        } else {
            uint32_t src = 0U, dst = 0U;

            rc = ipv4_parse_endpoints(buffer, len, &src, &dst) == 0
                     ? mesh_send(dst, NULL, buffer, len) : 0;
        }
        if (rc == 1) {
            return 0;
        }
    }
#endif
    if (!client_state.cur.valid) {
        return 0;   /* no session: the packet is dropped quietly */
    }
#ifdef CAMEX_L2
    if (current_config.l2) {
        return l2_send_frame(buffer, len, client_l2_send, NULL);
    }
#endif
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
    client_pake_new_connection();
    __atomic_store_n(&client_state.established, 0U, __ATOMIC_RELEASE);

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

    /* --keepalive 0 turns off frequent probes, but the server still needs
     * a sign of life before it expires the client; a lost link is then
     * noticed later (client_loss_after, #121) */
    if (current_config.keepalive == 0) {
        return CAMEX_KEEPALIVE_MAX;
    }
    if (cap < 1) {
        cap = 1;
    }
    /* Alive against a server with the lowest client_timeout= (#143) */
    if (cap > CAMEX_KEEPALIVE_MAX) {
        cap = CAMEX_KEEPALIVE_MAX;
    }
    return current_config.keepalive < cap ? current_config.keepalive : cap;
}

/* Seconds without an answer after which the link counts as lost: the
 * server timeout, as a keepalive goes at least every half of it; with
 * --keepalive 0 (one every CAMEX_KEEPALIVE_MAX) a whole interval
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

int client_key_is_pinned(void)
{
    return client_key_from_file;
}
