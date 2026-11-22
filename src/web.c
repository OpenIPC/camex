/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * web.c — the start page and nginx's lookup served by the server itself
 * (#203, web.h). Plain HTTP/1.0, one request per connection, GET and HEAD
 * only, in the server's own select() loop: no threads, no blocking. The
 * other servers on this host are asked through their control sockets in
 * the background, every WEB_PEER_EVERY seconds; a slow one only makes its
 * part of the page older.
 *
 */

#include "web.h"

#if !defined(CAMEX_NO_WEB) && !defined(_WIN32)

#include "camex.h"
#include "config.h"
#include "control.h"
#include "keys.h"
#include "log.h"
#include "monocypher.h"
#include "nginxmap.h"
#include "server.h"
#include "util.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* macOS has no MSG_NOSIGNAL: SO_NOSIGPIPE on each socket instead */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define WEB_CONNS        16
#define WEB_HEAD_MAX     8192U   /* a request's head; more: 431 */
#define WEB_TIMEOUT      10      /* seconds for a whole request */
#define WEB_HEAD_TIMEOUT 3       /* seconds for its head to come */
#define WEB_PW_PER_SEC   2       /* password checks (argon2) a second, all
                                  * addresses: the loop is the tunnel's */
#define WEB_PEERS        16
#define WEB_PEER_EVERY   2       /* seconds between rounds of the others */
#define WEB_PEER_TIMEOUT 2       /* seconds for one answer of another */
#define WEB_PEER_MAX     (4U << 20)   /* the longest answer kept */
#define WEB_PEER_STALE   15      /* seconds: an older answer is not shown */
#define WEB_AUTH_CACHE   4
#define WEB_FAIL_SLOTS   64
#define WEB_FAIL_MAX     5       /* wrong passwords in a row, then ... */
#define WEB_FAIL_BLOCK   60      /* ... seconds of 429 for that address */

typedef struct {
    int fd;                 /* -1 = unused */
    uint32_t ip;            /* the other side, host order */
    char in[WEB_HEAD_MAX + 1U];
    size_t in_len;
    char *out;
    size_t out_len;
    size_t out_off;
    time_t since;
} web_conn_t;

/* What the other servers answered: one command each */
enum { PEER_WEB, PEER_INDEX, PEER_JSON, PEER_CMDS };
static const char *const peer_cmd[PEER_CMDS] = {
    "CLIENTS WEB\n", "CLIENTS INDEX VIA\n", "STATUS JSON\n"
};

typedef struct {
    char path[108];
    int fd;                 /* asking now, -1 = not */
    int cmd;                /* what it is asked */
    char *buf;              /* the answer so far */
    size_t len;
    size_t cap;
    time_t since;
    char *got[PEER_CMDS];   /* its last whole answers, without the mark */
    time_t got_at;
} web_peer_t;

static int web_fd = -1;
static char web_spec[64];          /* web_listen= in effect */
static web_conn_t web_conns[WEB_CONNS];
static int web_conns_ready = 0;
static web_peer_t web_peers[WEB_PEERS];
static int web_npeers = 0;
static time_t web_peer_round = 0;

/* Passwords that matched: BLAKE2b of the header under a key of this run,
 * so the argon2 of web_password= runs once per browser, not per request */
static uint8_t web_auth_key[32];
static int web_auth_key_ok = 0;
static uint8_t web_auth_ok[WEB_AUTH_CACHE][32];
static int web_auth_n = 0;
static char web_auth_for[128];     /* the web_password= they matched */

typedef struct {
    uint32_t ip;
    unsigned int fails;
    time_t until;
    time_t last;            /* its last wrong password */
} web_fail_t;
static time_t web_pw_second = 0;
static unsigned int web_pw_count = 0;
static web_fail_t web_fails[WEB_FAIL_SLOTS];

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

#ifdef SO_NOSIGPIPE
    {
        int one = 1;

        (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    }
#endif
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int is_loopback(uint32_t ip)
{
    return (ip >> 24) == 127U;
}

static void conn_close(web_conn_t *c)
{
    if (c->fd >= 0) {
        close(c->fd);
    }
    free(c->out);
    c->fd = -1;
    c->out = NULL;
    c->in_len = 0U;
    c->out_len = 0U;
    c->out_off = 0U;
}

static void conns_init(void)
{
    size_t i;

    if (web_conns_ready) {
        return;
    }
    for (i = 0U; i < WEB_CONNS; ++i) {
        web_conns[i].fd = -1;
        web_conns[i].out = NULL;
    }
    for (i = 0U; i < WEB_PEERS; ++i) {
        web_peers[i].fd = -1;
    }
    web_conns_ready = 1;
}

/* ---------------------------------------------------------------- peers */

static void peer_stop(web_peer_t *p)
{
    if (p->fd >= 0) {
        close(p->fd);
    }
    p->fd = -1;
    free(p->buf);
    p->buf = NULL;
    p->len = 0U;
    p->cap = 0U;
}

static void peer_forget(web_peer_t *p)
{
    int k;

    peer_stop(p);
    for (k = 0; k < PEER_CMDS; ++k) {
        free(p->got[k]);
        p->got[k] = NULL;
    }
}

static void peers_clear(void)
{
    int i;

    for (i = 0; i < web_npeers; ++i) {
        peer_forget(&web_peers[i]);
    }
    web_npeers = 0;
}

/* Ask p for its command p->cmd: connect and send (a unix socket takes
 * the few bytes at once) */
static void peer_ask(web_peer_t *p)
{
    struct sockaddr_un sa;
    const char *cmd = peer_cmd[p->cmd];
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0) {
        return;
    }
    if (fd >= FD_SETSIZE || set_nonblock(fd) != 0) {
        close(fd);
        return;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", p->path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        int err = errno;

        close(fd);
        /* Nobody there (a server gone, its socket left behind): its
         * clients are not shown or resolved any more. Busy (its backlog
         * full): its last answer stays until WEB_PEER_STALE (#210) */
        if (err == ECONNREFUSED || err == ENOENT) {
            peer_forget(p);
        }
        return;
    }
    if (send(fd, cmd, strlen(cmd), MSG_NOSIGNAL) != (ssize_t)strlen(cmd)) {
        close(fd);
        return;
    }
    p->fd = fd;
    p->len = 0U;
    p->since = time(NULL);
}

/* A new round: who the others are now, then each asked its first
 * command */
static void peers_round(time_t now)
{
    char (*paths)[108] = NULL;
    int n, i, j;
    web_peer_t keep[WEB_PEERS];
    int nkeep = 0;

    web_peer_round = now;
    n = control_peers(&paths);
    /* Who stays keeps what it said; who is gone is forgotten */
    for (i = 0; i < web_npeers; ++i) {
        int still = 0;

        for (j = 0; j < n; ++j) {
            if (strcmp(paths[j], web_peers[i].path) == 0) {
                still = 1;
                break;
            }
        }
        if (still && nkeep < WEB_PEERS) {
            peer_stop(&web_peers[i]);
            keep[nkeep++] = web_peers[i];
        } else {
            peer_forget(&web_peers[i]);
        }
    }
    for (j = 0; j < n && nkeep < WEB_PEERS; ++j) {
        int known = 0;

        for (i = 0; i < nkeep; ++i) {
            if (strcmp(keep[i].path, paths[j]) == 0) {
                known = 1;
                break;
            }
        }
        if (!known) {
            memset(&keep[nkeep], 0, sizeof(keep[nkeep]));
            keep[nkeep].fd = -1;
            snprintf(keep[nkeep].path, sizeof(keep[nkeep].path), "%s",
                     paths[j]);
            nkeep++;
        }
    }
    free(paths);
    memcpy(web_peers, keep, (size_t)nkeep * sizeof(keep[0]));
    web_npeers = nkeep;
    for (i = 0; i < web_npeers; ++i) {
        web_peers[i].cmd = PEER_WEB;
        peer_ask(&web_peers[i]);
    }
}

/* p's answer is whole: kept, then its next command */
static void peer_answered(web_peer_t *p)
{
    size_t m = sizeof(CONTROL_END_MARK) - 1U;
    char *text = p->buf;

    p->buf = NULL;
    text[p->len - m] = '\0';
    free(p->got[p->cmd]);
    /* An older server does not know CLIENTS INDEX VIA: ERROR, nothing */
    if (strncmp(text, "ERROR", 5) == 0) {
        free(text);
        p->got[p->cmd] = NULL;
    } else {
        p->got[p->cmd] = text;
    }
    peer_stop(p);
    if (++p->cmd < PEER_CMDS) {
        peer_ask(p);
    } else {
        p->got_at = time(NULL);
    }
}

static void peer_read(web_peer_t *p)
{
    char chunk[4096];
    ssize_t n = recv(p->fd, chunk, sizeof(chunk), 0);
    size_t m = sizeof(CONTROL_END_MARK) - 1U;

    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return;
    }
    if (n <= 0) {
        /* Closed: whole only with the end mark after a line */
        if (p->buf != NULL && p->len >= m &&
            memcmp(p->buf + p->len - m, CONTROL_END_MARK, m) == 0 &&
            (p->len == m || p->buf[p->len - m - 1U] == '\n')) {
            peer_answered(p);
        } else {
            peer_stop(p);
        }
        return;
    }
    if (p->len + (size_t)n + 1U > p->cap) {
        size_t cap = p->cap == 0U ? 8192U : p->cap * 2U;
        char *grown;

        while (cap < p->len + (size_t)n + 1U) {
            cap *= 2U;
        }
        if (cap > WEB_PEER_MAX || (grown = realloc(p->buf, cap)) == NULL) {
            peer_stop(p);
            return;
        }
        p->buf = grown;
        p->cap = cap;
    }
    memcpy(p->buf + p->len, chunk, (size_t)n);
    p->len += (size_t)n;
    p->buf[p->len] = '\0';
}

/* ------------------------------------------------------------- the data */

typedef struct {
    char *p;
    size_t len;
    size_t cap;
    int err;
} web_buf_t;

static void wb_add(web_buf_t *b, const char *s, size_t n)
{
    if (b->err || s == NULL) {
        return;
    }
    if (b->len + n + 1U > b->cap) {
        size_t cap = b->cap == 0U ? 4096U : b->cap;
        char *grown;

        while (cap < b->len + n + 1U) {
            cap *= 2U;
        }
        grown = realloc(b->p, cap);
        if (grown == NULL) {
            b->err = 1;
            return;
        }
        b->p = grown;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void wb_str(web_buf_t *b, const char *s)
{
    if (s != NULL) {
        wb_add(b, s, strlen(s));
    }
}

/* What p last said for kind, unless that is too old (a server that stopped
 * answering) */
static char *peer_got(const web_peer_t *p, int kind)
{
    if (p->got_at == 0 || difftime(time(NULL), p->got_at) > (double)WEB_PEER_STALE) {
        return NULL;
    }
    return p->got[kind];
}

/* This server's records of kind (a PEER_ command) and, unless web_scope =
 * self, those of the others: malloc'd text, NULL without memory */
static char *gather(int kind)
{
    web_buf_t b = { NULL, 0U, 0U, 0 };
    size_t len = 0U;
    char *own = NULL;
    int i;

    switch (kind) {
    case PEER_WEB:
        own = server_clients_list(0, 1, &len);
        break;
    case PEER_INDEX:
        own = server_clients_index(1, &len);
        break;
    default:
        own = server_status(1, &len);
        break;
    }
    if (kind == PEER_JSON) {
        int any = 0;

        wb_str(&b, "{\"servers\":[");
        if (own != NULL) {
            while (len > 0U && (own[len - 1U] == '\n' || own[len - 1U] == '\r')) {
                own[--len] = '\0';
            }
            wb_add(&b, own, len);
            any = 1;
        }
        for (i = 0; !server_db.globals.web_scope_self && i < web_npeers; ++i) {
            char *t = peer_got(&web_peers[i], PEER_JSON);
            size_t tl;

            if (t == NULL) {
                continue;
            }
            tl = strlen(t);
            while (tl > 0U && (t[tl - 1U] == '\n' || t[tl - 1U] == '\r')) {
                tl--;
            }
            if (any) {
                wb_str(&b, ",");
            }
            wb_add(&b, t, tl);
            any = 1;
        }
        wb_str(&b, "]}\n");
    } else {
        wb_add(&b, own, len);
        for (i = 0; !server_db.globals.web_scope_self && i < web_npeers; ++i) {
            const char *t = peer_got(&web_peers[i], kind);

            if (t != NULL) {
                if (b.len > 0U && b.p[b.len - 1U] != '\n') {
                    wb_str(&b, "\n");
                }
                wb_str(&b, t);
            }
        }
        if (b.p == NULL && !b.err) {
            wb_str(&b, "");
        }
    }
    free(own);
    if (b.err) {
        free(b.p);
        return NULL;
    }
    return b.p;
}

/* ------------------------------------------------------------- requests */

/* A header line of that name in the head, whatever its value or length */
static int strcasestr_head(const char *head, const char *needle)
{
    size_t n = strlen(needle);
    const char *p;

    for (p = head; *p != '\0'; ++p) {
        if (strncasecmp(p, needle, n) == 0) {
            return 1;
        }
    }
    return 0;
}

/* The value of header name in the head (case-insensitive), copied into
 * out without blanks around it; 1 if it is there */
static int header(const char *head, const char *name, char *out, size_t size)
{
    size_t nl = strlen(name);
    const char *p = strstr(head, "\r\n");

    while (p != NULL && p[2] != '\0' && !(p[2] == '\r' && p[3] == '\n')) {
        const char *line = p + 2;
        const char *eol = strstr(line, "\r\n");

        if (eol == NULL) {
            break;
        }
        if ((size_t)(eol - line) > nl && line[nl] == ':' &&
            strncasecmp(line, name, nl) == 0) {
            const char *v = line + nl + 1;
            size_t vl;

            while (v < eol && (*v == ' ' || *v == '\t')) {
                v++;
            }
            vl = (size_t)(eol - v);
            while (vl > 0U && (v[vl - 1U] == ' ' || v[vl - 1U] == '\t')) {
                vl--;
            }
            if (vl >= size) {
                return 0;
            }
            memcpy(out, v, vl);
            out[vl] = '\0';
            return 1;
        }
        p = eol;
    }
    return 0;
}

static void respond(web_conn_t *c, int code, const char *reason,
                    const char *type, const char *extra, const char *body,
                    size_t blen, int head_only)
{
    web_buf_t b = { NULL, 0U, 0U, 0 };
    char line[256];

    snprintf(line, sizeof(line), "HTTP/1.0 %d %s\r\n", code, reason);
    wb_str(&b, line);
    wb_str(&b, "Server: camex\r\nConnection: close\r\n"
               "Cache-Control: no-cache\r\n"
               "X-Content-Type-Options: nosniff\r\n"
               "Referrer-Policy: same-origin\r\n");
    if (type != NULL) {
        snprintf(line, sizeof(line), "Content-Type: %s\r\n", type);
        wb_str(&b, line);
    }
    snprintf(line, sizeof(line), "Content-Length: %lu\r\n",
             (unsigned long)blen);
    wb_str(&b, line);
    wb_str(&b, extra);
    wb_str(&b, "\r\n");
    if (!head_only) {
        wb_add(&b, body, blen);
    }
    if (b.err) {
        free(b.p);
        conn_close(c);
        return;
    }
    c->out = b.p;
    c->out_len = b.len;
    c->out_off = 0U;
}

static void respond_text(web_conn_t *c, int code, const char *reason,
                         const char *extra, const char *text, int head_only)
{
    respond(c, code, reason, "text/plain; charset=utf-8", extra, text,
            strlen(text), head_only);
}

static web_fail_t *fail_slot(uint32_t ip, int create)
{
    web_fail_t *free_slot = NULL, *oldest = NULL;
    time_t now = time(NULL);
    size_t i;

    for (i = 0U; i < WEB_FAIL_SLOTS; ++i) {
        web_fail_t *f = &web_fails[i];
        int in_use = f->fails > 0U || f->until > now;

        if (in_use && f->ip == ip) {
            return f;
        }
        if (!in_use) {
            if (free_slot == NULL) {
                free_slot = f;
            }
        } else if (f->until <= now &&
                   (oldest == NULL || f->last < oldest->last)) {
            oldest = f;   /* counting, not shut out: the oldest goes */
        }
    }
    if (!create) {
        return NULL;
    }
    /* A free slot first; never one that shuts an address out now, nor
     * one another address counts in while a free one is left (#210) */
    if (free_slot == NULL) {
        free_slot = oldest;
    }
    if (free_slot == NULL) {
        return NULL;   /* every slot shuts one out: no count for this one */
    }
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->ip = ip;
    return free_slot;
}

/* A wrong password from ip (or one not checked: the budget was spent);
 * WEB_FAIL_MAX in a row shut it out for WEB_FAIL_BLOCK */
static void count_fail(uint32_t ip, time_t now)
{
    web_fail_t *f = fail_slot(ip, 1);

    if (f == NULL) {
        return;
    }
    f->last = now;
    if (++f->fails >= WEB_FAIL_MAX) {
        f->fails = 0U;
        f->until = now + WEB_FAIL_BLOCK;
        log_message(LOG_WARNING, "Start page: %u wrong passwords from "
                    "%u.%u.%u.%u, refused for %d s", WEB_FAIL_MAX,
                    (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 255U),
                    (unsigned)((ip >> 8) & 255U), (unsigned)(ip & 255U),
                    WEB_FAIL_BLOCK);
    }
}

/* 1: the request may see the page; 0: an answer (401, 429) is set */
static int authorized(web_conn_t *c, const char *pw, int head_only)
{
    char auth[512], pass[384];
    uint8_t dec[384], h[32];
    const char *colon;
    web_fail_t *f;
    time_t now = time(NULL);
    int n, i;

    if (pw[0] == '\0' || strcmp(pw, "none") == 0) {
        return 1;
    }
    f = fail_slot(c->ip, 0);
    if (f != NULL && f->until > now) {
        respond_text(c, 429, "Too Many Requests", "Retry-After: 60\r\n",
                     "Too many wrong passwords: wait a minute\n", head_only);
        return 0;
    }
    if (strcmp(web_auth_for, pw) != 0) {
        /* Another web_password= (a reload): what matched before is void */
        web_auth_n = 0;
        snprintf(web_auth_for, sizeof(web_auth_for), "%s", pw);
    }
    if (!web_auth_key_ok) {
        if (get_random_bytes(web_auth_key, sizeof(web_auth_key)) != 0) {
            respond_text(c, 500, "Internal Server Error", NULL,
                         "No randomness\n", head_only);
            return 0;
        }
        web_auth_key_ok = 1;
    }
    if (header(c->in, "Authorization", auth, sizeof(auth)) &&
        strncasecmp(auth, "Basic ", 6) == 0) {
        crypto_blake2b_keyed(h, sizeof(h), web_auth_key, sizeof(web_auth_key),
                             (const uint8_t *)auth, strlen(auth));
        for (i = 0; i < web_auth_n; ++i) {
            if (crypto_verify32(h, web_auth_ok[i]) == 0) {
                return 1;
            }
        }
        /* A new header costs an argon2 (8 MiB) in the tunnel's loop: a
         * few a second for all addresses together (#210) */
        if (now != web_pw_second) {
            web_pw_second = now;
            web_pw_count = 0U;
        }
        if (++web_pw_count > WEB_PW_PER_SEC) {
            /* Not checked, but counted: a flood of guesses shuts its
             * address out as wrong ones would */
            count_fail(c->ip, now);
            respond_text(c, 429, "Too Many Requests", "Retry-After: 1\r\n",
                         "Busy: try again in a second\n", head_only);
            return 0;
        }
        n = key_base64_decode(auth + 6, dec, sizeof(dec) - 1U);
        if (n > 0) {
            dec[n] = '\0';
            colon = memchr(dec, ':', (size_t)n);
            /* Any user name: only the password counts */
            if (colon != NULL) {
                snprintf(pass, sizeof(pass), "%s", colon + 1);
                if (key_password_check(pass, pw)) {
                    crypto_wipe(pass, sizeof(pass));
                    crypto_wipe(dec, sizeof(dec));
                    if (web_auth_n < WEB_AUTH_CACHE) {
                        web_auth_n++;
                    } else {
                        memmove(web_auth_ok[0], web_auth_ok[1],
                                (WEB_AUTH_CACHE - 1U) * 32U);
                    }
                    memcpy(web_auth_ok[web_auth_n - 1], h, 32U);
                    f = fail_slot(c->ip, 0);
                    if (f != NULL) {
                        memset(f, 0, sizeof(*f));
                    }
                    return 1;
                }
                crypto_wipe(pass, sizeof(pass));
            }
        }
        crypto_wipe(dec, sizeof(dec));
        count_fail(c->ip, now);
    }
    respond_text(c, 401, "Unauthorized",
                 "WWW-Authenticate: Basic realm=\"camex\", charset=\"UTF-8\"\r\n",
                 "A password is needed\n", head_only);
    return 0;
}

/* The page: behind nginx (a request from 127.0.0.1 with X-Forwarded-Host)
 * its links name the devices; else they are the tunnel addresses */
static void page(web_conn_t *c, int head_only)
{
    char fwd[300], proto[16];
    const char *domain = server_db.globals.domain;
    int via_nginx = is_loopback(c->ip) &&
                    header(c->in, "X-Forwarded-Host", fwd, sizeof(fwd));
    int http_port = 80;
    char *index = gather(PEER_INDEX), *web = NULL, *html;
    size_t count = 0U;
    int dport = server_db.globals.device_port > 0
                    ? server_db.globals.device_port : 80;

    if (via_nginx) {
        const char *colon = strrchr(fwd, ':');

        /* nginx's port in the links unless the page came over HTTPS */
        if (!(header(c->in, "X-Forwarded-Proto", proto, sizeof(proto)) &&
              strcasecmp(proto, "https") == 0) &&
            colon != NULL && strchr(colon, ']') == NULL) {
            http_port = atoi(colon + 1);
        }
    } else {
        web = gather(PEER_WEB);
    }
    if (index == NULL) {
        free(web);
        respond_text(c, 503, "Service Unavailable", NULL, "Out of memory\n",
                     head_only);
        return;
    }
    html = web_index_format(index, web, domain, http_port,
                            via_nginx ? 0 : dport, NULL, &count);
    free(index);
    free(web);
    if (html == NULL) {
        respond_text(c, 500, "Internal Server Error", NULL,
                     "Cannot build the page\n", head_only);
        return;
    }
    respond(c, 200, "OK", "text/html; charset=utf-8", NULL, html, strlen(html),
            head_only);
    free(html);
}

static void resolve(web_conn_t *c, int head_only)
{
    char host[300], up[40], extra[80];
    char *web;
    int dport = server_db.globals.device_port > 0
                    ? server_db.globals.device_port : 80;

    /* nginx's subrequest on this host only, not a page's request passed
     * on (X-Forwarded-Host): the names and addresses stay here */
    if (!is_loopback(c->ip) ||
        strcasestr_head(c->in, "\r\nX-Forwarded-Host:")) {
        respond_text(c, 404, "Not Found", NULL, "Not found\n", head_only);
        return;
    }
    up[0] = '\0';
    if (header(c->in, "X-Camex-Host", host, sizeof(host)) &&
        (web = gather(PEER_WEB)) != NULL) {
        (void)web_resolve(web, server_db.globals.domain, dport, host, up,
                          sizeof(up));
        free(web);
    }
    /* 204 with where it goes; 403 when no device has the name, which
     * nginx's auth_request turns into its offline page (error_page 403) */
    if (up[0] == '\0') {
        respond(c, 403, "Forbidden", NULL, NULL, "", 0U, 1);
        return;
    }
    snprintf(extra, sizeof(extra), "X-Camex-Upstream: %s\r\n", up);
    respond(c, 204, "No Content", NULL, extra, "", 0U, 1);
}

static void request(web_conn_t *c)
{
    char method[8], target[512], version[16];
    const char *path;
    int head_only;
    char *json;

    if (sscanf(c->in, "%7s %511s %15s", method, target, version) != 3 ||
        strncmp(version, "HTTP/1.", 7) != 0) {
        respond_text(c, 400, "Bad Request", NULL, "Bad request\n", 0);
        return;
    }
    head_only = strcmp(method, "HEAD") == 0;
    if (strcmp(method, "GET") != 0 && !head_only) {
        respond_text(c, 405, "Method Not Allowed", "Allow: GET, HEAD\r\n",
                     "Only GET and HEAD\n", 0);
        return;
    }
    path = target;
    /* The query is not looked at */
    target[strcspn(target, "?#")] = '\0';
    if (strcmp(path, "/resolve") == 0) {
        resolve(c, head_only);
        return;
    }
    if (strcmp(path, "/") != 0 && strcmp(path, "/index.html") != 0 &&
        strcmp(path, "/status.json") != 0) {
        respond_text(c, 404, "Not Found", NULL, "Not found\n", head_only);
        return;
    }
    if (!authorized(c, server_db.globals.web_password, head_only)) {
        return;
    }
    if (strcmp(path, "/status.json") == 0) {
        json = gather(PEER_JSON);
        if (json == NULL) {
            respond_text(c, 503, "Service Unavailable", NULL,
                         "Out of memory\n", head_only);
            return;
        }
        respond(c, 200, "OK", "application/json", NULL, json, strlen(json),
                head_only);
        free(json);
        return;
    }
    page(c, head_only);
}

/* -------------------------------------------------------------- the loop */

static int web_listen_on(const char *spec)
{
    struct sockaddr_in sa;
    char ip[16];
    const char *colon = strrchr(spec, ':');
    int fd, one = 1;

    if (colon == NULL || (size_t)(colon - spec) >= sizeof(ip)) {
        return -1;
    }
    memcpy(ip, spec, (size_t)(colon - spec));
    ip[colon - spec] = '\0';
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)atoi(colon + 1));
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) {
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        log_message(LOG_ERR, "Start page: socket: %s", strerror(errno));
        return -1;
    }
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (fd >= FD_SETSIZE || bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
        listen(fd, WEB_CONNS) != 0 || set_nonblock(fd) != 0) {
        log_message(LOG_ERR, "Start page: cannot listen on %s: %s (web_listen= "
                    "in [server])", spec, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

void web_close(void)
{
    size_t i;

    conns_init();
    for (i = 0U; i < WEB_CONNS; ++i) {
        conn_close(&web_conns[i]);
    }
    peers_clear();
    if (web_fd >= 0) {
        close(web_fd);
    }
    web_fd = -1;
    web_spec[0] = '\0';
}

int web_apply(void)
{
    const camex_server_globals_t *g = &server_db.globals;
    uint32_t ip;
    int fd;
    char ipt[16];
    const char *colon;
    struct in_addr a;

    conns_init();
    /* A reload that drops web_password= from a LAN page closes it (#210):
     * the rule below holds for a page that runs as for a new one */
    if (web_fd >= 0 && strcmp(g->web_listen, web_spec) == 0) {
        colon = strrchr(g->web_listen, ':');
        snprintf(ipt, sizeof(ipt), "%.*s",
                 colon != NULL ? (int)(colon - g->web_listen) : 0,
                 g->web_listen);
        if (inet_pton(AF_INET, ipt, &a) == 1 &&
            !is_loopback(ntohl(a.s_addr)) && g->web_password[0] == '\0') {
            log_message(LOG_ERR, "web_listen = %s is reachable from the "
                        "network and web_password= is gone: the start page "
                        "is closed", g->web_listen);
            web_close();
            return -1;
        }
        return 0;
    }
    if (strcmp(g->web_listen, web_spec) == 0 && g->web_listen[0] == '\0') {
        return 0;
    }
    web_close();
    if (g->web_listen[0] == '\0') {
        return 0;
    }
    colon = strrchr(g->web_listen, ':');
    snprintf(ipt, sizeof(ipt), "%.*s",
             colon != NULL ? (int)(colon - g->web_listen) : 0, g->web_listen);
    if (inet_pton(AF_INET, ipt, &a) != 1) {
        return -1;
    }
    ip = ntohl(a.s_addr);
    /* Beyond this host, a password or a page open on purpose */
    if (!is_loopback(ip) && g->web_password[0] == '\0') {
        log_message(LOG_ERR, "web_listen = %s is reachable from the network: "
                    "it needs web_password= (camex --hash-password), or "
                    "web_password = none for a page open to all",
                    g->web_listen);
        return -1;
    }
    fd = web_listen_on(g->web_listen);
    if (fd < 0) {
        return -1;
    }
    web_fd = fd;
    snprintf(web_spec, sizeof(web_spec), "%s", g->web_listen);
    if (!is_loopback(ip) && strcmp(g->web_password, "none") == 0) {
        log_message(LOG_WARNING, "Start page on %s without a password "
                    "(web_password = none): anyone who reaches it sees the "
                    "devices", g->web_listen);
    }
    log_message(LOG_INFO, "Start page: http://%s/%s", g->web_listen,
                is_loopback(ip) ? " (and /resolve for nginx)" : "");
    return 0;
}

int web_fdset(fd_set *readset, fd_set *writeset, int maxfd)
{
    time_t now = time(NULL);
    size_t i;
    int k;

    if (web_fd < 0) {
        return maxfd;
    }
    FD_SET(web_fd, readset);
    if (web_fd > maxfd) {
        maxfd = web_fd;
    }
    for (i = 0U; i < WEB_CONNS; ++i) {
        web_conn_t *c = &web_conns[i];

        if (c->fd < 0) {
            continue;
        }
        /* A head not complete in WEB_HEAD_TIMEOUT, a whole request not
         * done in WEB_TIMEOUT: closed (#210) */
        if (difftime(now, c->since) > (double)WEB_TIMEOUT ||
            (c->out == NULL &&
             difftime(now, c->since) > (double)WEB_HEAD_TIMEOUT)) {
            conn_close(c);
            continue;
        }
        if (c->out != NULL) {
            FD_SET(c->fd, writeset);
        } else {
            FD_SET(c->fd, readset);
        }
        if (c->fd > maxfd) {
            maxfd = c->fd;
        }
    }
    if (!server_db.globals.web_scope_self &&
        difftime(now, web_peer_round) >= (double)WEB_PEER_EVERY) {
        peers_round(now);
    }
    for (k = 0; k < web_npeers; ++k) {
        web_peer_t *p = &web_peers[k];

        if (p->fd < 0) {
            continue;
        }
        if (difftime(now, p->since) > (double)WEB_PEER_TIMEOUT) {
            peer_stop(p);    /* its part stays as it last was */
            continue;
        }
        FD_SET(p->fd, readset);
        if (p->fd > maxfd) {
            maxfd = p->fd;
        }
    }
    return maxfd;
}

void web_handle(const fd_set *readset, const fd_set *writeset)
{
    size_t i;
    int k;

    if (web_fd < 0) {
        return;
    }
    for (k = 0; k < web_npeers; ++k) {
        if (web_peers[k].fd >= 0 && FD_ISSET(web_peers[k].fd, readset)) {
            peer_read(&web_peers[k]);
        }
    }
    if (FD_ISSET(web_fd, readset)) {
        for (;;) {
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            int fd = accept(web_fd, (struct sockaddr *)&from, &fl);
            web_conn_t *c = NULL;

            if (fd < 0) {
                break;
            }
            for (i = 0U; i < WEB_CONNS; ++i) {
                if (web_conns[i].fd < 0) {
                    c = &web_conns[i];
                    break;
                }
            }
            /* All taken: the oldest one still sending its head makes room,
             * one from beyond this host first, so idle visitors cannot keep
             * nginx's lookups out (#210) */
            if (c == NULL) {
                web_conn_t *victim = NULL;
                int pass;

                /* nginx (127.0.0.1) may take any one; one from the
                 * network only another one from the network */
                int passes = is_loopback(ntohl(from.sin_addr.s_addr)) ? 2 : 1;

                for (pass = 0; pass < passes && victim == NULL; ++pass) {
                    for (i = 0U; i < WEB_CONNS; ++i) {
                        web_conn_t *o = &web_conns[i];

                        if (o->out == NULL &&
                            (pass == 1 || !is_loopback(o->ip)) &&
                            (victim == NULL || o->since < victim->since)) {
                            victim = o;
                        }
                    }
                }
                if (victim != NULL) {
                    conn_close(victim);
                    c = victim;
                }
            }
            if (c == NULL || fd >= FD_SETSIZE || set_nonblock(fd) != 0 ||
                from.sin_family != AF_INET) {
                close(fd);
                continue;
            }
            c->fd = fd;
            c->ip = ntohl(from.sin_addr.s_addr);
            c->in_len = 0U;
            c->out = NULL;
            c->since = time(NULL);
        }
    }
    for (i = 0U; i < WEB_CONNS; ++i) {
        web_conn_t *c = &web_conns[i];

        if (c->fd < 0) {
            continue;
        }
        if (c->out == NULL && FD_ISSET(c->fd, readset)) {
            ssize_t n = recv(c->fd, c->in + c->in_len,
                             WEB_HEAD_MAX - c->in_len, 0);

            if (n <= 0) {
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                              errno == EINTR)) {
                    continue;
                }
                conn_close(c);
                continue;
            }
            c->in_len += (size_t)n;
            c->in[c->in_len] = '\0';
            if (strstr(c->in, "\r\n\r\n") != NULL) {
                request(c);
            } else if (c->in_len >= WEB_HEAD_MAX) {
                respond_text(c, 431, "Request Header Fields Too Large", NULL,
                             "Request head too large\n", 0);
            }
        } else if (c->out != NULL && FD_ISSET(c->fd, writeset)) {
            ssize_t n = send(c->fd, c->out + c->out_off,
                             c->out_len - c->out_off, MSG_NOSIGNAL);

            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    continue;
                }
                conn_close(c);
                continue;
            }
            c->out_off += (size_t)n;
            if (c->out_off >= c->out_len) {
                conn_close(c);
            }
        }
    }
}

#endif /* !CAMEX_NO_WEB && !_WIN32 */
