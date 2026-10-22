/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * v2tool.c — protocol v2 handshakes for the e2e tests (#17)
 *
 *   v2tool <host> <port> <server-pubkey> <psk> udp|tcp <who>...
 *
 *   v2tool serve <port> <server-private-key> <psk> <first> <later>
 *
 * <who> is "auto:<client ID>[@<address hint>]" or "manual:<tunnel IP>".
 * Each handshake
 * uses its own socket; for each one a line "<who> <result>" is printed:
 * the LOCAL_CIDR of the server's CONFIG (auto), "OK" (manual), or "-".
 * PACE_MS=<n> waits n ms between handshakes (the server limits them per
 * second). With HOLD=1 in the environment the sockets stay open and are never
 * read again (a client that stops reading), until the process is killed;
 * with GO_FILE=<path> as well, the sending side is closed (FIN) once that
 * file exists. RCVBUF=<bytes> sets a small receive buffer. NOWAIT=1 only
 * sends each INIT and does not wait for the reply (a flood, #78).
 *
 */

#include "keys.h"
#include "monocypher.h"
#include "noise.h"
#include "session.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

/* Globals normally provided by camex.c */
#include "camex.h"
camex_config_t current_config;
volatile sig_atomic_t running = 1;
time_t g_now = 0;

static int open_socket(const char *host, const char *port, int tcp)
{
    struct addrinfo hints, *res;
    int fd;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = tcp ? SOCK_STREAM : SOCK_DGRAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) {
        return -1;
    }
    fd = socket(AF_INET, hints.ai_socktype, 0);
    if (fd >= 0 && getenv("RCVBUF") != NULL) {
        int rcvbuf = atoi(getenv("RCVBUF"));

        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    }
    if (fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static int send_packet(int fd, int tcp, const uint8_t *p, size_t len)
{
    uint8_t buf[2048];
    size_t off = 0U;

    if (!tcp) {
        return send(fd, p, len, 0) == (ssize_t)len ? 0 : -1;
    }
    if (len + 2U > sizeof(buf)) {
        return -1;
    }
    buf[0] = (uint8_t)(len >> 8);
    buf[1] = (uint8_t)len;
    memcpy(buf + 2, p, len);
    /* A stream: send() may take part of the frame */
    while (off < len + 2U) {
        ssize_t n = send(fd, buf + off, len + 2U - off, 0);

        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

/* One packet within 2 s, or -1 */
static ssize_t recv_packet(int fd, int tcp, uint8_t *p, size_t size)
{
    fd_set rs;
    struct timeval tv = { 2, 0 };
    uint8_t hdr[2];
    size_t want, got = 0U;

    FD_ZERO(&rs);
    FD_SET(fd, &rs);
    if (select(fd + 1, &rs, NULL, NULL, &tv) <= 0) {
        return -1;
    }
    if (!tcp) {
        return recv(fd, p, size, 0);
    }
    if (recv(fd, hdr, 2, MSG_WAITALL) != 2) {
        return -1;
    }
    want = ((size_t)hdr[0] << 8) | hdr[1];
    if (want > size) {
        return -1;
    }
    while (got < want) {
        ssize_t n = recv(fd, p + got, want - got, 0);
        if (n <= 0) {
            return -1;
        }
        got += (size_t)n;
    }
    return (ssize_t)want;
}

static void handshake(const char *host, const char *port, int tcp,
                      const uint8_t pub[32], const uint8_t psk[32],
                      const char *who, int *hold_fd)
{
    noise_hs_t hs;
    uint8_t payload[512], pkt[1024], plain[1024], seed[32];
    char text[256];
    size_t tlen, len, plen = 0U;
    ssize_t n;
    uint32_t idx = v2_new_index();
    int fd = open_socket(host, port, tcp);
    const char *result = "-";
    char cidr[64];

    *hold_fd = -1;
    if (fd < 0) {
        printf("%s -\n", who);
        return;
    }
    if (strncmp(who, "auto:", 5) == 0) {
        /* auto:<id>@<ip> also asks for that address (LOCAL_IP= hint) */
        const char *at = strchr(who + 5, '@');

        if (at != NULL) {
            snprintf(text, sizeof(text), "CAMEX REGISTER VER=2 MODE=AUTO "
                     "CLIENT_ID=%.*s LOCAL_IP=%s\n", (int)(at - (who + 5)),
                     who + 5, at + 1);
        } else {
            snprintf(text, sizeof(text), "CAMEX REGISTER VER=2 MODE=AUTO "
                     "CLIENT_ID=%s\n", who + 5);
        }
    } else {
        snprintf(text, sizeof(text), "CAMEX REGISTER VER=2 MODE=MANUAL "
                 "LOCAL_IP=%s\n", who + 7);
    }
    tlen = strlen(text);
    v2_timestamp(payload);
    memcpy(payload + V2_TIMESTAMP_LEN, text, tlen);
    get_random_bytes(seed, sizeof(seed));
    noise_init_initiator(&hs, (const uint8_t *)CAMEX_NOISE_PROLOGUE,
                         sizeof(CAMEX_NOISE_PROLOGUE) - 1U, pub);
    memcpy(pkt, CAMEX_MAGIC, 4);
    pkt[4] = V2_TYPE_INIT;
    v2_write_u32(pkt + 5, idx);
    len = noise_write_msg1(&hs, psk, seed, payload, V2_TIMESTAMP_LEN + tlen,
                           pkt + V2_INIT_HDR,
                           sizeof(pkt) - V2_INIT_HDR - V2_MAC_LEN);
    if (len > 0U) {
        len += V2_INIT_HDR;
        v2_mac1(pub, pkt, len, pkt + len);
        len += V2_MAC_LEN;
    }
    if (len > 0U && send_packet(fd, tcp, pkt, len) == 0 &&
        getenv("NOWAIT") == NULL &&
        (n = recv_packet(fd, tcp, pkt, sizeof(pkt))) > 0 &&
        v2_packet_type(pkt, (size_t)n) == V2_TYPE_RESP &&
        v2_read_u32(pkt + 9) == idx &&
        noise_read_msg2(&hs, pkt + V2_RESP_HDR, (size_t)n - V2_RESP_HDR,
                        plain, sizeof(plain) - 1U, &plen) == 0) {
        char *p;

        plain[plen] = '\0';
        p = strstr((char *)plain, "LOCAL_CIDR=");
        if (p != NULL) {
            sscanf(p + 11, "%63s", cidr);
            result = cidr;
        } else if (strstr((char *)plain, " OK") != NULL) {
            result = "OK";
        }
    }
    noise_wipe(&hs);
    printf("%s %s\n", who, result);
    fflush(stdout);
    *hold_fd = fd;
}

/*
 * A fake server for client tests (UDP): answers every handshake with the
 * CONFIG text given (\n for newlines) — the first handshake with first,
 * the later ones with later — and nothing else (no DATA, no keepalive
 * answers). For CONFIGs a real server would never send (#12).
 */
static int serve(const char *port, const char *priv_b64, const char *psk_text,
                 const char *first, const char *later)
{
    uint8_t priv[32], pub[32], psk[32];
    struct sockaddr_in addr;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    unsigned int count = 0U;

    if (fd < 0 || key_server_from_b64(priv_b64, priv, pub) != 0 ||
        key_from_psk(psk_text, psk) != 0) {
        fprintf(stderr, "serve: bad socket, key or PSK\n");
        return 2;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)atoi(port));
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("serve: bind");
        return 2;
    }
    for (;;) {
        uint8_t pkt[2048], resp[2048], plain[1024], es[32], seed[32];
        char text[1024];
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        noise_hs_t hs;
        size_t plen = 0U, rlen, i, t = 0U;
        const char *conf;
        ssize_t n = recvfrom(fd, pkt, sizeof(pkt), 0,
                             (struct sockaddr *)&from, &flen);

        if (n <= 0 || v2_packet_type(pkt, (size_t)n) != V2_TYPE_INIT ||
            (size_t)n < V2_INIT_HDR + V2_MAC_LEN) {
            continue;   /* DATA and anything else: no answer */
        }
        n -= V2_MAC_LEN;
        noise_init_responder(&hs, (const uint8_t *)CAMEX_NOISE_PROLOGUE,
                             sizeof(CAMEX_NOISE_PROLOGUE) - 1U, priv, pub);
        if (noise_msg1_es(&hs, pkt + V2_INIT_HDR, (size_t)n - V2_INIT_HDR,
                          es) != 0 ||
            noise_read_msg1(&hs, psk, es, pkt + V2_INIT_HDR,
                            (size_t)n - V2_INIT_HDR, plain, sizeof(plain),
                            &plen) != 0) {
            noise_wipe(&hs);
            continue;
        }
        conf = (count++ == 0U) ? first : later;
        for (i = 0U; conf[i] != '\0' && t + 1U < sizeof(text); ++i) {
            if (conf[i] == '\\' && conf[i + 1U] == 'n') {
                text[t++] = '\n';
                ++i;
            } else {
                text[t++] = conf[i];
            }
        }
        text[t] = '\0';
        get_random_bytes(seed, sizeof(seed));
        memcpy(resp, CAMEX_MAGIC, 4);
        resp[4] = V2_TYPE_RESP;
        v2_write_u32(resp + 5, 0x5e5e5e5eU);
        memcpy(resp + 9, pkt + 5, 4);   /* the client's index */
        rlen = noise_write_msg2(&hs, seed, (const uint8_t *)text, t,
                                resp + V2_RESP_HDR,
                                sizeof(resp) - V2_RESP_HDR);
        noise_wipe(&hs);
        if (rlen > 0U) {
            sendto(fd, resp, V2_RESP_HDR + rlen, 0,
                   (struct sockaddr *)&from, flen);
            printf("answered handshake %u\n", count);
            fflush(stdout);
        }
    }
}

int main(int argc, char **argv)
{
    uint8_t pub[32], psk[32];
    int tcp, i;
    int *fds;
    int hold = getenv("HOLD") != NULL;

    if (argc == 7 && strcmp(argv[1], "serve") == 0) {
        return serve(argv[2], argv[3], argv[4], argv[5], argv[6]);
    }
    if (argc < 7 || key_parse_b64(argv[3], pub) != 0 ||
        key_from_psk(argv[4], psk) != 0) {
        fprintf(stderr, "usage: v2tool <host> <port> <server-pubkey> <psk> "
                "udp|tcp auto:<id>[@<ip>]|manual:<ip>...\n"
                "       v2tool serve <port> <server-private-key> <psk> "
                "<first CONFIG> <later CONFIG>\n");
        return 2;
    }
    tcp = strcmp(argv[5], "tcp") == 0;
    fds = calloc((size_t)argc, sizeof(int));
    for (i = 6; i < argc; ++i) {
        handshake(argv[1], argv[2], tcp, pub, psk, argv[i], &fds[i]);
        if (getenv("PACE_MS") != NULL) {   /* stay under the server's limit */
            usleep((useconds_t)atoi(getenv("PACE_MS")) * 1000U);
        }
    }
    if (hold) {
        /* GO_FILE: once it exists, close the sending side (FIN) */
        const char *go = getenv("GO_FILE");

        while (go != NULL && access(go, F_OK) != 0) {
            usleep(200000);
        }
        for (i = 6; go != NULL && i < argc; ++i) {
            if (fds[i] >= 0) {
                shutdown(fds[i], SHUT_WR);
            }
        }
        for (;;) {
            pause();
        }
    }
    for (i = 6; i < argc; ++i) {
        if (fds[i] >= 0) {
            close(fds[i]);
        }
    }
    free(fds);
    return 0;
}
