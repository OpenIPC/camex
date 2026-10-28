/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * nginxmap.c — an nginx map of the live clients (#84) and their start
 * page (#105)
 *
 */

#include "nginxmap.h"
#include "version.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAP_HOST_MAX 253   /* a host name, without its NUL */

typedef struct {
    char host[MAP_HOST_MAX + 1];
    uint32_t addr;         /* host order, for the order of duplicates */
    char addr_text[16];
    char label[64];        /* the ID in lower case */
    /* The start page only (#105, #110) */
    char name[CAMEX_NAME_MAX + 1];   /* "" = none */
    uint8_t live;          /* connected (the map's records always are) */
    uint8_t conf;          /* has a [client ID] block */
    long since;            /* seconds on this connection */
    long last;             /* seconds since its last packet, -1 = none */
    long hs_age;           /* seconds since its last handshake */
    unsigned long hs;      /* handshakes */
    unsigned long long rx; /* bytes */
    unsigned long long tx;
    char peer[16];         /* its external IP (#119), "" = not known */
    long peer_port;        /* the port of its link, 0 = no link */
} map_entry_t;

typedef struct {
    char *p;
    size_t len;
    size_t cap;
    int err;
} map_buf_t;

/* Append, growing the buffer as needed; sets err without memory */
static void buf_printf(map_buf_t *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void buf_printf(map_buf_t *b, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (b->err) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        b->err = 1;
        return;
    }
    if (b->len + (size_t)n + 1U > b->cap) {
        size_t cap = b->cap == 0U ? 1024U : b->cap;
        char *grown;

        while (b->len + (size_t)n + 1U > cap) {
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
    va_start(ap, fmt);
    vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

/* A host label: 1-63 of a-z, 0-9 and '-', not starting or ending with '-';
 * label is in lower case already */
static int valid_label(const char *label, size_t n)
{
    size_t i;

    if (n == 0U || n > 63U || label[0] == '-' || label[n - 1U] == '-') {
        return 0;
    }
    for (i = 0U; i < n; ++i) {
        unsigned char c = (unsigned char)label[i];

        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '-')) {
            return 0;
        }
    }
    return 1;
}

/* A host name: labels (any case) separated by single dots */
static int valid_hostname(const char *name)
{
    size_t n = strlen(name), start = 0U, i;
    char low[MAP_HOST_MAX + 1];

    if (n == 0U || n > MAP_HOST_MAX) {
        return 0;
    }
    for (i = 0U; i <= n; ++i) {
        low[i] = (char)tolower((unsigned char)name[i]);
    }
    for (i = 0U; i <= n; ++i) {
        if (low[i] == '.' || low[i] == '\0') {
            if (!valid_label(low + start, i - start)) {
                return 0;
            }
            start = i + 1U;
        }
    }
    return 1;
}

int camex_domain_valid(const char *domain)
{
    return domain != NULL && valid_hostname(domain);
}

/* A dotted IPv4 address: four numbers 0..255, nothing else (digits and
 * dots only: sscanf alone would take blanks and signs) */
static int parse_addr(const char *a, uint32_t *out)
{
    unsigned int b[4];
    int used = 0;

    if (strspn(a, "0123456789.") != strlen(a) ||
        sscanf(a, "%3u.%3u.%3u.%3u%n", &b[0], &b[1], &b[2], &b[3],
               &used) != 4 || a[used] != '\0' || !isdigit((unsigned char)a[0]) ||
        b[0] > 255U || b[1] > 255U || b[2] > 255U || b[3] > 255U) {
        return 0;
    }
    *out = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
    return 1;
}

/* <IPv4 address>:<port> for --default: the example config proxies to the
 * map's value without a resolver, so a host name would not resolve. The
 * map gets it written out anew (norm), never the text as given */
static int valid_target(const char *t, char *norm, size_t size)
{
    const char *colon = strrchr(t, ':');
    char host[MAP_HOST_MAX + 1];
    char *end = NULL;
    long port;
    uint32_t a;

    if (colon == NULL || colon == t ||
        (size_t)(colon - t) > MAP_HOST_MAX) {
        return 0;
    }
    memcpy(host, t, (size_t)(colon - t));
    host[colon - t] = '\0';
    if (colon[1] == '\0' || strlen(colon + 1) > 5U ||
        strspn(colon + 1, "0123456789") != strlen(colon + 1)) {
        return 0;
    }
    port = strtol(colon + 1, &end, 10);
    if (*end != '\0' || port < 1L || port > 65535L || !parse_addr(host, &a)) {
        return 0;
    }
    snprintf(norm, size, "%u.%u.%u.%u:%ld", (unsigned)(a >> 24),
             (unsigned)((a >> 16) & 255U), (unsigned)((a >> 8) & 255U),
             (unsigned)(a & 255U), port);
    return 1;
}

/* A key that is one of map's own parameters: nginx drops the quotes before
 * map reads it, so only a leading backslash keeps it a host (nginx docs) */
static int map_reserved(const char *h)
{
    return strcmp(h, "default") == 0 || strcmp(h, "hostnames") == 0 ||
           strcmp(h, "include") == 0 || strcmp(h, "volatile") == 0;
}

/* A client ID for a warning: control and non-ASCII bytes as '?', so an ID
 * cannot drive the terminal */
static const char *warn_id(char *buf, size_t size, const char *id)
{
    size_t i;

    for (i = 0U; id[i] != '\0' && i + 1U < size; ++i) {
        unsigned char ch = (unsigned char)id[i];

        buf[i] = (ch < 0x20U || ch >= 0x7fU) ? '?' : (char)ch;
    }
    buf[i] = '\0';
    return buf;
}

static int cmp_entry(const void *a, const void *b)
{
    const map_entry_t *x = a, *y = b;
    int c = strcmp(x->host, y->host);

    /* The same ID twice: the lower address (numerically) first, and of
     * one address the lower name, so that the one kept does not depend on
     * the servers' order */
    if (c != 0) {
        return c;
    }
    /* The start page: a live record before a configured one that is not */
    if (x->live != y->live) {
        return x->live ? -1 : 1;
    }
    if (x->addr != y->addr) {
        return x->addr < y->addr ? -1 : 1;
    }
    return strcmp(x->name, y->name);
}

int camex_name_valid(const char *name)
{
    const unsigned char *p = (const unsigned char *)name;
    size_t n = strlen(name);

    if (n == 0U || n > CAMEX_NAME_MAX) {
        return 0;
    }
    while (*p != '\0') {
        uint32_t cp;
        size_t len, k;

        if (*p < 0x80U) {
            if (*p < 0x20U || *p == 0x7fU) {
                return 0;
            }
            p++;
            continue;
        }
        if (*p >= 0xc2U && *p <= 0xdfU) {
            len = 2U;
            cp = *p & 0x1fU;
        } else if (*p >= 0xe0U && *p <= 0xefU) {
            len = 3U;
            cp = *p & 0x0fU;
        } else if (*p >= 0xf0U && *p <= 0xf4U) {
            len = 4U;
            cp = *p & 0x07U;
        } else {
            return 0;   /* a continuation byte, C0/C1 or above F4 */
        }
        for (k = 1U; k < len; ++k) {
            if ((p[k] & 0xc0U) != 0x80U) {
                return 0;   /* also the NUL at the end */
            }
            cp = (cp << 6) | (p[k] & 0x3fU);
        }
        if ((len == 3U && cp < 0x800U) || (len == 4U && cp < 0x10000U) ||
            cp > 0x10ffffU || (cp >= 0xd800U && cp <= 0xdfffU) ||
            (cp >= 0x80U && cp <= 0x9fU)) {
            return 0;   /* overlong, out of range, a surrogate, C1 */
        }
        p += len;
    }
    return 1;
}

/* A number field of a record: 0..max, digits only */
static int parse_num(const char *f, unsigned long long max,
                     unsigned long long *out)
{
    char *end = NULL;

    if (f[0] == '\0' || strspn(f, "0123456789") != strlen(f) ||
        strlen(f) > 20U) {
        return 0;
    }
    errno = 0;
    *out = strtoull(f, &end, 10);
    return errno == 0 && *end == '\0' && *out <= max;
}

/* Split line at tabs into at most max fields; their number */
static int split_tabs(char *line, char **f, int max)
{
    int n = 0;

    f[n++] = line;
    for (; *line != '\0' && n < max; ++line) {
        if (*line == '\t') {
            *line = '\0';
            f[n++] = line + 1;
        }
    }
    return n;
}

/* One record into *e: for the map (index 0) exactly "<ID> <address>",
 * for the start page (index 1) a CLIENTS INDEX record (server.h). 1 if
 * it is a client to list; 0 for anything else, with a warning on warn
 * for an ID that cannot be a host name */
static int parse_record(const char *p, size_t ll, const char *domain,
                        size_t dlen, int index, FILE *warn, map_entry_t *e)
{
    char line[512], id[64], addr[32], extra[2], shown[64];
    const char *name = NULL;
    size_t k;
    uint32_t a = 0U;
    int has_addr = 1;      /* "-": a configured client with no address */

    if (ll == 0U || ll >= sizeof(line)) {
        return 0;
    }
    memcpy(line, p, ll);
    line[ll] = '\0';
    memset(e, 0, sizeof(*e));
    e->live = 1U;
    e->last = -1;
    if (!index) {
        if (sscanf(line, "%63s %31s %1s", id, addr, extra) != 2 ||
            !parse_addr(addr, &a)) {
            return 0;
        }
    } else {
        char *f[13];
        int n = split_tabs(line, f, 13);
        unsigned long long v[8];
        int live = strcmp(f[0], "L") == 0;
        /* The peer and its port (#119) make 12 or 13 fields; a server
         * before them gives 10 or 11 */
        int peered = live && n >= 12;

        if ((!live && strcmp(f[0], "O") != 0) || (live && n < 10) ||
            (!live && n < 3) || strlen(f[1]) >= sizeof(id) ||
            (live && n > 13) || (!live && n > 4)) {
            return 0;
        }
        snprintf(id, sizeof(id), "%s", f[1]);
        if (strcmp(f[2], "-") == 0 && !live) {
            a = 0U;
            has_addr = 0;
        } else if (!parse_addr(f[2], &a)) {
            return 0;
        }
        e->live = live ? 1U : 0U;
        e->conf = 1U;
        if (live) {
            /* configured, connected, last packet, last handshake,
             * handshakes, rx, tx */
            if (!parse_num(f[3], 1U, &v[0]) ||
                !parse_num(f[4], 0x7fffffffULL, &v[1]) ||
                (strcmp(f[5], "-") != 0 &&
                 !parse_num(f[5], 0x7fffffffULL, &v[2])) ||
                !parse_num(f[6], 0x7fffffffULL, &v[3]) ||
                !parse_num(f[7], 0xffffffffULL, &v[4]) ||
                !parse_num(f[8], ~0ULL, &v[5]) ||
                !parse_num(f[9], ~0ULL, &v[6])) {
                return 0;
            }
            e->conf = (uint8_t)v[0];
            e->since = (long)v[1];
            e->last = strcmp(f[5], "-") == 0 ? -1 : (long)v[2];
            e->hs_age = (long)v[3];
            e->hs = (unsigned long)v[4];
            e->rx = v[5];
            e->tx = v[6];
            if (peered) {
                uint32_t pa;

                if ((strcmp(f[10], "-") != 0 && !parse_addr(f[10], &pa)) ||
                    (strcmp(f[11], "-") != 0 &&
                     (!parse_num(f[11], 65535U, &v[7]) || v[7] == 0U))) {
                    return 0;
                }
                if (strcmp(f[10], "-") != 0) {
                    snprintf(e->peer, sizeof(e->peer), "%u.%u.%u.%u",
                             (unsigned)(pa >> 24),
                             (unsigned)((pa >> 16) & 255U),
                             (unsigned)((pa >> 8) & 255U),
                             (unsigned)(pa & 255U));
                    e->peer_port = strcmp(f[11], "-") != 0 ? (long)v[7] : 0L;
                }
                name = n > 12 ? f[12] : NULL;
            } else {
                name = n > 10 ? f[10] : NULL;
            }
        } else {
            name = n > 3 ? f[3] : NULL;
        }
    }
    for (k = 0U; id[k] != '\0'; ++k) {
        e->label[k] = (char)tolower((unsigned char)id[k]);
    }
    e->label[k] = '\0';
    if (!valid_label(e->label, k)) {
        if (warn != NULL) {
            fprintf(warn, "camex: client ID %s is not a host name label: "
                    "left out of the %s\n",
                    warn_id(shown, sizeof(shown), id),
                    index ? "start page" : "map");
        }
        return 0;
    }
    if (dlen > 0U && k + 1U + dlen > MAP_HOST_MAX) {
        if (warn != NULL) {
            fprintf(warn, "camex: %s.%s is longer than a host name: left "
                    "out of the %s\n", e->label, domain,
                    index ? "start page" : "map");
        }
        return 0;
    }
    if (dlen > 0U) {
        snprintf(e->host, sizeof(e->host), "%s.%s", e->label, domain);
    } else {
        snprintf(e->host, sizeof(e->host), "%s", e->label);
    }
    for (k = 0U; e->host[k] != '\0'; ++k) {
        e->host[k] = (char)tolower((unsigned char)e->host[k]);
    }
    e->addr = a;
    if (has_addr) {
        snprintf(e->addr_text, sizeof(e->addr_text), "%u.%u.%u.%u",
                 (unsigned)(a >> 24), (unsigned)((a >> 16) & 255U),
                 (unsigned)((a >> 8) & 255U), (unsigned)(a & 255U));
    }
    /* The server checked the name when it loaded its file; one that is
     * not valid now (another program on the socket) is shown as the ID */
    if (name != NULL && name[0] != '\0') {
        if (camex_name_valid(name)) {
            snprintf(e->name, sizeof(e->name), "%s", name);
        } else if (warn != NULL) {
            fprintf(warn, "camex: the name of client %s is not valid text: "
                    "the ID is shown\n", e->label);
        }
    }
    return 1;
}

/* All records of lines, sorted by host and then address, in *out
 * (malloc'd; NULL when there are none). Their number, or -1 without
 * memory */
static long parse_records(const char *lines, const char *domain,
                          int index, FILE *warn, map_entry_t **out)
{
    map_entry_t *e = NULL;
    size_t n = 0U, cap = 0U;
    size_t dlen = domain != NULL ? strlen(domain) : 0U;
    const char *p = lines != NULL ? lines : "";

    *out = NULL;
    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        size_t ll = eol != NULL ? (size_t)(eol - p) : strlen(p);

        if (n == cap) {
            map_entry_t *grown;

            cap = cap == 0U ? 64U : cap * 2U;
            grown = realloc(e, cap * sizeof(*e));
            if (grown == NULL) {
                free(e);
                return -1;
            }
            e = grown;
        }
        if (parse_record(p, ll, domain, dlen, index, warn, &e[n])) {
            n++;
        }
        p += ll + (eol != NULL ? 1U : 0U);
    }
    if (n > 1U) {
        qsort(e, n, sizeof(*e), cmp_entry);
    }
    *out = e;
    return (long)n;
}

char *nginx_map_format(const char *lines, const char *domain, int port,
                       const char *dflt, FILE *warn, size_t *count)
{
    map_entry_t *e = NULL;
    size_t dlen = domain != NULL ? strlen(domain) : 0U;
    map_buf_t out = { NULL, 0U, 0U, 0 };
    char dnorm[32] = "";
    long n, i;

    *count = 0U;
    if ((dlen > 0U && !valid_hostname(domain)) || port < 1 || port > 65535 ||
        (dflt != NULL && dflt[0] != '\0' &&
         !valid_target(dflt, dnorm, sizeof(dnorm)))) {
        return NULL;
    }
    n = parse_records(lines, domain, 0, warn, &e);
    if (n < 0) {
        return NULL;
    }

    buf_printf(&out, "# camex --status --format nginx-map: generated, do "
               "not edit\n");
    /* For the sync script's checks (#106): the domain the names end in */
    buf_printf(&out, "# domain: %s\n", dlen > 0U ? domain : "(none)");
    buf_printf(&out, "map $host $camex_upstream {\n");
    if (dnorm[0] != '\0') {
        buf_printf(&out, "    default %s;\n", dnorm);
    }
    for (i = 0; i < n; ++i) {
        /* The same ID on two servers: the lower address is kept */
        if (i > 0 && strcmp(e[i].host, e[i - 1].host) == 0) {
            if (warn != NULL) {
                fprintf(warn, "camex: %s is listed twice (two servers?): "
                        "only %s kept\n", e[i].host, e[i - 1].addr_text);
            }
            continue;
        }
        /* Quoted; an ID such as "default" or "include" also needs the
         * backslash to stay a host (without --domain) */
        buf_printf(&out, "    \"%s%s\" %s:%d;\n",
                   map_reserved(e[i].host) ? "\\" : "", e[i].host,
                   e[i].addr_text, port);
        (*count)++;
    }
    buf_printf(&out, "}\n");
    free(e);
    if (out.err) {
        free(out.p);
        return NULL;
    }
    return out.p;
}

/* The text a row is listed by: its name, or the ID without one */
static const char *shown_name(const map_entry_t *e)
{
    return e->name[0] != '\0' ? e->name : e->label;
}

/* By name, letters A-Z in any case alike (bytes otherwise), then by ID:
 * the order does not depend on the servers' order, the locale or the
 * case of a name */
static int cmp_shown(const void *a, const void *b)
{
    const map_entry_t *x = a, *y = b;
    const unsigned char *p = (const unsigned char *)shown_name(x);
    const unsigned char *q = (const unsigned char *)shown_name(y);
    int c;

    while (*p != '\0' && tolower(*p) == tolower(*q)) {
        p++;
        q++;
    }
    c = tolower(*p) - tolower(*q);
    /* Names equal but for the case: by ID (hosts are unique here) */
    return c != 0 ? c : strcmp(x->host, y->host);
}

/* Text for HTML: the five characters with a meaning there as entities.
 * Names are valid UTF-8 without control characters already */
static void buf_html(map_buf_t *b, const char *s)
{
    for (; *s != '\0'; ++s) {
        switch (*s) {
        case '&':
            buf_printf(b, "&amp;");
            break;
        case '<':
            buf_printf(b, "&lt;");
            break;
        case '>':
            buf_printf(b, "&gt;");
            break;
        case '"':
            buf_printf(b, "&quot;");
            break;
        case '\'':
            buf_printf(b, "&#39;");
            break;
        default:
            buf_printf(b, "%c", *s);
            break;
        }
    }
}

/* A duration as camex --status writes it: 42s, 3m07s, 5h02m, 2d04h */
static void buf_dur(map_buf_t *b, long v)
{
    if (v < 0L) {
        v = 0L;
    }
    if (v < 60L) {
        buf_printf(b, "%lds", v);
    } else if (v < 3600L) {
        buf_printf(b, "%ldm%02lds", v / 60L, v % 60L);
    } else if (v < 86400L) {
        buf_printf(b, "%ldh%02ldm", v / 3600L, (v % 3600L) / 60L);
    } else {
        buf_printf(b, "%ldd%02ldh", v / 86400L, (v % 86400L) / 3600L);
    }
}

/* Bytes in KiB: one decimal below 10 KiB, whole numbers above with a
 * narrow space between thousands (714 096) */
static void buf_kib(map_buf_t *b, unsigned long long v)
{
    char d[24];
    size_t n, i;

    if (v < 10240ULL) {
        buf_printf(b, "%llu.%llu", v / 1024ULL,
                   (v % 1024ULL) * 10ULL / 1024ULL);
        return;
    }
    /* Rounded without v + 512, which would wrap near the top */
    snprintf(d, sizeof(d), "%llu",
             v / 1024ULL + ((v % 1024ULL) >= 512ULL ? 1ULL : 0ULL));
    n = strlen(d);
    for (i = 0U; i < n; ++i) {
        if (i > 0U && (n - i) % 3U == 0U) {
            buf_printf(b, "&#8239;");
        }
        buf_printf(b, "%c", d[i]);
    }
}

/* One list of the page: a table of e[0..n), or a line saying it is empty */
static void buf_list(map_buf_t *b, const char *cls, const map_entry_t *e,
                     long n, const char *empty, const char *port)
{
    long i;

    buf_printf(b, "<div class=\"list %s\">\n", cls);
    if (n == 0) {
        buf_printf(b, "<div class=\"empty\">%s</div>\n</div>\n", empty);
        return;
    }
    buf_printf(b, "<div class=\"rack\">\n<table>\n<thead><tr><th></th>"
               "<th>Name</th><th class=\"id\">ID</th><th class=\"addr\">"
               "Address</th><th class=\"addr\">Peer</th><th class=\"conn\">"
               "Connected</th><th>Last "
               "packet</th><th class=\"hs\">Handshake</th><th>RX / TX KiB"
               "</th></tr></thead>\n<tbody>\n");
    for (i = 0; i < n; ++i) {
        const map_entry_t *d = &e[i];

        /* The host and the ID are host name labels: nothing to escape
         * in them */
        if (d->live) {
            buf_printf(b, "<tr class=\"dev\"><td class=\"lamp\"><span "
                       "class=\"bulb\"></span></td><td><a href=\"%s//%s%s/\">",
                       port[0] != '\0' ? "http:" : "", d->host, port);
            buf_html(b, shown_name(d));
            buf_printf(b, "</a></td><td class=\"id\">%s</td><td "
                       "class=\"addr\">%s</td><td class=\"addr\">",
                       d->label, d->addr_text);
            /* Its external IP, a link to its own web port (#119) */
            if (d->peer[0] == '\0') {
                buf_printf(b, "&mdash;");
            } else if (d->peer_port == 0L) {
                buf_printf(b, "%s", d->peer);
            } else if (d->peer_port == 80L) {
                buf_printf(b, "<a class=\"peer\" href=\"http://%s/\" "
                           "target=\"_blank\" rel=\"noopener\">%s</a>",
                           d->peer, d->peer);
            } else {
                buf_printf(b, "<a class=\"peer\" href=\"http://%s:%ld/\" "
                           "target=\"_blank\" rel=\"noopener\">%s</a>",
                           d->peer, d->peer_port, d->peer);
            }
            buf_printf(b, "</td><td class=\"conn\">");
            buf_dur(b, d->since);
            buf_printf(b, "</td><td>");
            if (d->last < 0L) {
                buf_printf(b, "&mdash;");
            } else {
                buf_dur(b, d->last);
                buf_printf(b, " ago");
            }
            buf_printf(b, "</td><td class=\"hs\">");
            buf_dur(b, d->hs_age);
            buf_printf(b, " ago &middot; %lu</td><td class=\"tr\">", d->hs);
            buf_kib(b, d->rx);
            buf_printf(b, " / ");
            buf_kib(b, d->tx);
            buf_printf(b, "</td></tr>\n");
        } else {
            /* Not connected: no link, nginx has no name for it */
            buf_printf(b, "<tr class=\"dev off\"><td class=\"lamp\"><span "
                       "class=\"bulb\"></span></td><td><span class=\"nm\">");
            buf_html(b, shown_name(d));
            buf_printf(b, "</span></td><td class=\"id\">%s</td><td "
                       "class=\"addr\">%s</td><td class=\"addr\">&mdash;"
                       "</td><td class=\"conn\">offline"
                       "</td><td>&mdash;</td><td class=\"hs\">&mdash;</td>"
                       "<td>&mdash;</td></tr>\n", d->label,
                       d->addr_text[0] != '\0' ? d->addr_text : "&mdash;");
        }
    }
    buf_printf(b, "</tbody>\n</table>\n</div>\n</div>\n");
}

char *nginx_index_format(const char *lines, const char *domain,
                         int http_port, FILE *warn, size_t *count)
{
    /* The links' port: nginx's, when it is not HTTP's own (#141). Then
     * the links say http: the port is that of HTTP, not of HTTPS */
    char port[8] = "";
    map_entry_t *e = NULL, *conf = NULL, *autos = NULL;
    size_t dlen = domain != NULL ? strlen(domain) : 0U;
    map_buf_t out = { NULL, 0U, 0U, 0 };
    long n, i, kept = 0, nconf = 0, nauto = 0, live = 0, off = 0;

    *count = 0U;
    if (dlen > 0U && !valid_hostname(domain)) {
        return NULL;
    }
    n = parse_records(lines, domain, 1, warn, &e);
    if (n < 0) {
        return NULL;
    }
    /* The same ID on two servers: a live record before a configured one
     * that is not connected, then the lower address and name, as in the
     * map */
    for (i = 0; i < n; ++i) {
        if (i > 0 && strcmp(e[i].host, e[kept - 1].host) == 0) {
            /* A [client ID] block on any server makes it Configured, and
             * its name= names it */
            e[kept - 1].conf |= e[i].conf;
            if (e[kept - 1].name[0] == '\0' && e[i].name[0] != '\0') {
                memcpy(e[kept - 1].name, e[i].name, sizeof(e[i].name));
            }
            if (warn != NULL && e[i].live) {
                fprintf(warn, "camex: %s is listed twice (two servers?): "
                        "only %s kept\n", e[i].host, e[kept - 1].addr_text);
            }
            continue;
        }
        e[kept++] = e[i];
    }
    /* Configured: every [client ID] block, connected or not; Auto: the
     * connected clients without one. Each sorted by name, then ID */
    if (kept > 0) {
        conf = malloc((size_t)kept * sizeof(*conf));
        autos = malloc((size_t)kept * sizeof(*autos));
        if (conf == NULL || autos == NULL) {
            free(conf);
            free(autos);
            free(e);
            return NULL;
        }
    }
    for (i = 0; i < kept; ++i) {
        if (e[i].conf) {
            conf[nconf++] = e[i];
        } else {
            autos[nauto++] = e[i];
        }
        live += e[i].live ? 1 : 0;
        off += e[i].live ? 0 : 1;
    }
    free(e);
    if (nconf > 1) {
        qsort(conf, (size_t)nconf, sizeof(*conf), cmp_shown);
    }
    if (nauto > 1) {
        qsort(autos, (size_t)nauto, sizeof(*autos), cmp_shown);
    }

    /* The look of OpenIPC/vectis's page: brass, rivets and lamps */
    buf_printf(&out,
        "<!doctype html>\n"
        "<!-- camex --status --format nginx-index: generated, do not edit "
        "-->\n"
        "<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width,"
        "initial-scale=1\">\n"
        /* The script below refreshes the page in place (#142); without
         * it, a reload every 30 s */
        "<noscript><meta http-equiv=\"refresh\" content=\"30\"></noscript>\n"
        "<meta name=\"camex-version\" content=\"" CAMEX_VERSION "\">\n"
        "<title>Camex Devices</title>\n"
        "<style>\n"
        ":root{--panel:#2c2119;--panel2:#3d2d22;--brass:#c9a24d;"
        "--brass2:#8c6a2a;--amber:#ffb347;--ink:#e8d9b8;--dim:#8a7455;"
        "--green:#5ad38f;--red:#c8452c}\n"
        "*{box-sizing:border-box}\n"
        "body{margin:0;min-height:100vh;background:radial-gradient(ellipse "
        "at 50%% 0,#3b2c22 0,#1a1512 55%%,#0f0b09 100%%);color:var(--ink);"
        "font-family:Georgia,'Times New Roman',serif;display:flex;"
        "flex-direction:column;align-items:center;padding:28px 16px}\n"
        "h1{font-size:clamp(22px,4.5vw,40px);letter-spacing:.2em;"
        "text-transform:uppercase;margin:0 0 6px;text-align:center;"
        "color:var(--brass);text-shadow:0 1px 0 #f3d78a,0 -1px 0 #3d2a0c,"
        "0 0 22px rgba(201,162,77,.35)}\n"
        ".sub{font-family:'Courier New',monospace;font-size:12px;"
        "color:var(--dim);letter-spacing:.3em;text-transform:uppercase;"
        "margin-bottom:24px;text-align:center}\n"
        ".panel{position:relative;width:min(1200px,100%%);"
        "background:linear-gradient(180deg,var(--panel2),var(--panel));"
        "border:3px solid var(--brass2);border-radius:16px;"
        "box-shadow:inset 0 0 0 2px #1a1210,inset 0 0 50px rgba(0,0,0,.6),"
        "0 24px 48px rgba(0,0,0,.65);padding:30px 24px 24px}\n"
        ".rivet{position:absolute;width:14px;height:14px;border-radius:50%%;"
        "background:radial-gradient(circle at 35%% 35%%,#f1d38b,#8c6a2a 60%%,"
        "#3a2a10);box-shadow:0 1px 2px #000,inset 0 -1px 1px rgba(0,0,0,.5)}\n"
        ".rivet.tl{top:8px;left:8px}.rivet.tr{top:8px;right:8px}"
        ".rivet.bl{bottom:8px;left:8px}.rivet.br{bottom:8px;right:8px}\n"
        ".plate{font-family:'Courier New',monospace;font-size:12px;"
        "letter-spacing:.2em;text-transform:uppercase;color:#2a1c10;"
        "background:linear-gradient(180deg,#e2c377,#a88237);"
        "border:1px solid #5b4416;border-radius:4px;padding:4px 10px;"
        "display:inline-block;box-shadow:inset 0 1px 0 #fbe6a8,"
        "0 1px 2px #000}\n"
        ".top{display:flex;justify-content:space-between;align-items:center;"
        "flex-wrap:wrap;gap:10px}\n"
        ".tab{position:absolute;opacity:0;pointer-events:none}\n"
        ".tabs{display:flex;gap:10px;margin-top:20px;flex-wrap:wrap}\n"
        ".tabs label{cursor:pointer;font-family:'Courier New',monospace;"
        "font-size:12px;letter-spacing:.18em;text-transform:uppercase;"
        "color:var(--dim);padding:8px 14px;border:1px solid #574123;"
        "border-radius:8px;background:rgba(0,0,0,.25);"
        "box-shadow:inset 0 2px 6px rgba(0,0,0,.5)}\n"
        ".tabs label b{color:var(--ink);font-weight:normal}\n"
        ".tabs label i{font-style:normal;color:var(--red)}\n"
        "#tab-conf:checked~.tabs label[for=tab-conf],#tab-auto:checked~.tabs "
        "label[for=tab-auto]{color:#2a1c10;background:linear-gradient(180deg,"
        "#e2c377,#a88237);border-color:#5b4416;box-shadow:inset 0 1px 0 "
        "#fbe6a8,0 1px 2px #000}\n"
        "#tab-conf:checked~.tabs label[for=tab-conf] b,#tab-auto:checked~"
        ".tabs label[for=tab-auto] b{color:#2a1c10;font-weight:bold}\n"
        "#tab-conf:checked~.tabs label[for=tab-conf] i{color:#7a1a10}\n"
        ".tab:focus-visible~.tabs label{outline:1px dotted var(--dim)}\n"
        ".list{display:none}\n"
        "#tab-conf:checked~.list-conf,#tab-auto:checked~.list-auto"
        "{display:block}\n"
        ".rack{margin-top:14px;border:1px solid #574123;border-radius:10px;"
        "background:rgba(0,0,0,.25);box-shadow:inset 0 2px 8px rgba(0,0,0,.6);"
        "overflow-x:auto}\n"
        "table{width:100%%;border-collapse:collapse}\n"
        "th{font-family:'Courier New',monospace;font-size:11px;"
        "font-weight:normal;letter-spacing:.16em;text-transform:uppercase;"
        "color:var(--dim);text-align:left;padding:10px 10px;"
        "border-bottom:1px solid #574123;white-space:nowrap}\n"
        "td{padding:11px 10px;border-bottom:1px solid rgba(87,65,35,.5);"
        "vertical-align:middle;font-family:'Courier New',monospace;"
        "font-size:13px;color:var(--dim);white-space:nowrap}\n"
        "tr:last-child td{border-bottom:0}\n"
        "tbody tr:hover{background:rgba(201,162,77,.07)}\n"
        ".bulb{display:block;width:14px;height:14px;border-radius:50%%;"
        "border:2px solid #6e5320;background:radial-gradient(circle at 40%% "
        "35%%,#d8ffe8,var(--green) 45%%,#155b34);box-shadow:0 0 10px "
        "var(--green)}\n"
        ".off .bulb{background:radial-gradient(circle at 40%% 35%%,#ffd0c4,"
        "var(--red) 45%%,#5a1a0e);box-shadow:0 0 10px var(--red)}\n"
        "td.lamp{width:34px}\n"
        "td:nth-child(2){font-family:Georgia,'Times New Roman',serif;"
        "white-space:normal;min-width:9em}\n"
        "a{color:var(--amber);text-decoration:none;font-size:17px;"
        "letter-spacing:.04em;text-shadow:0 0 10px rgba(255,179,71,.35);"
        "word-break:break-word}\n"
        "a:hover{text-decoration:underline}\n"
        "a.peer{font-size:13px;letter-spacing:0;text-shadow:none}\n"
        ".nm{font-size:17px;letter-spacing:.04em;color:var(--dim)}\n"
        ".off td{opacity:.75}\n"
        ".empty{margin-top:14px;font-family:'Courier New',monospace;"
        "font-size:clamp(16px,3vw,22px);letter-spacing:.18em;"
        "text-transform:uppercase;color:var(--dim);padding:14px 16px;"
        "border:2px solid #574123;border-radius:8px;background:#120d0a;"
        "box-shadow:inset 0 0 20px rgba(0,0,0,.8);text-align:center}\n"
        ".foot{margin-top:18px;font-family:'Courier New',monospace;"
        "font-size:11px;color:#5e4d36;letter-spacing:.2em;"
        "text-transform:uppercase;text-align:center}\n"
        ".stale-mark{display:none;color:var(--red)}\n"
        "body.stale .stale-mark{display:inline}\n"
        "body.stale .panel{opacity:.7}\n"
        "@media (max-width:820px){.addr,.hs{display:none}}\n"
        "@media (max-width:560px){.conn,.id{display:none}.panel{padding:26px "
        "14px 18px}td,th{padding:10px 6px}td.tr,th{white-space:normal}"
        ".tabs{gap:6px}.tabs label{padding:7px 9px;letter-spacing:.08em}}\n"
        "</style>\n</head>\n<body>\n<h1>Camex Devices</h1>\n");
    buf_printf(&out, "<div class=\"sub\">%ld online &middot; %ld configured"
               "</div>\n"
               "<div class=\"panel\">\n"
               "<span class=\"rivet tl\"></span><span class=\"rivet tr\">"
               "</span><span class=\"rivet bl\"></span><span class=\"rivet "
               "br\"></span>\n"
               "<div class=\"top\"><span class=\"plate\">Device registry"
               "<span class=\"stale-mark\"> &middot; no connection</span>"
               "</span><span class=\"plate\">camex %s</span></div>\n",
               live, nconf, CAMEX_VERSION);
    /* The tabs: CSS only. Configured opens first when it has devices */
    buf_printf(&out, "<input type=\"radio\" name=\"tab\" class=\"tab\" "
               "id=\"tab-conf\"%s>\n<input type=\"radio\" name=\"tab\" "
               "class=\"tab\" id=\"tab-auto\"%s>\n"
               "<div class=\"tabs\"><label for=\"tab-conf\">Configured "
               "<b>%ld</b>", nconf > 0 || nauto == 0 ? " checked" : "",
               nconf > 0 || nauto == 0 ? "" : " checked", nconf);
    if (off > 0) {
        buf_printf(&out, " &middot; <i>%ld offline</i>", off);
    }
    buf_printf(&out, "</label><label for=\"tab-auto\">Auto <b>%ld</b>"
               "</label></div>\n", nauto);
    if (http_port > 0 && http_port != 80 && http_port <= 65535) {
        snprintf(port, sizeof(port), ":%d", http_port);
    }
    buf_list(&out, "list-conf", conf, nconf,
             "No [client ID] blocks in the server's file", port);
    buf_list(&out, "list-auto", autos, nauto, "No auto devices online",
             port);
    buf_printf(&out, "</div>\n<div class=\"foot\">Camex &middot; OpenIPC "
               "&middot; MIT License</div>\n"
               /* Remembers the chosen tab, and every 5 s puts the page
                * of now in place of this one (#142): the same file from
                * nginx, its counts and lists swapped, the tab and the
                * scroll kept. No answer: the data stays, marked "no
                * connection". Another camex version: one full reload.
                * The page works without it */
               "<script>\n"
               "(function(){\n"
               "var V=document.querySelector('meta[name=camex-version]')"
               ".content;\n"
               "function tab(){try{var t=localStorage.getItem('camex-tab'),"
               "r=t&&document.getElementById(t);if(r)r.checked=true}"
               "catch(e){}}\n"
               "tab();\n"
               "document.addEventListener('change',function(e){var x=e.target;"
               "if(x.classList&&x.classList.contains('tab')){try{localStorage"
               ".setItem('camex-tab',x.id)}catch(err){}}});\n"
               "function stale(on){document.body.classList.toggle('stale',on)}"
               "\n"
               /* Links with nginx's HTTP port (#141) on a page opened
                * over HTTPS: the HTTPS listener instead; a peer's link
                * (its own web port, #119) stays */
               "function tls(){if(location.protocol!=='https:')return;"
               "[].forEach.call(document.querySelectorAll("
               "'a[href^=\"http:\"]:not(.peer)'),"
               "function(x){var m=x.getAttribute('href').match("
               "/^http:\\/\\/([^\\/:]+):\\d+\\/$/);if(m)x.setAttribute('href',"
               "'https://'+m[1]+'/')})}\n"
               "tls();\n"
               /* The page's scroll and each list's own stay where they
                * were */
               "function swap(d){var a=document.querySelector('.panel'),"
               "b=d.querySelector('.panel'),s=document.querySelector('.sub'),"
               "t=d.querySelector('.sub');if(!a||!b)return;"
               "var c=document.querySelector('.tab:checked'),id=c&&c.id,"
               "X=window.scrollX,Y=window.scrollY,x={};"
               "[].forEach.call(document.querySelectorAll('.rack'),function(r){"
               "x[r.parentNode.className]=r.scrollLeft});"
               "a.innerHTML=b.innerHTML;if(s&&t)s.innerHTML=t.innerHTML;tls();"
               "var n=id&&document.getElementById(id);if(n)n.checked=true;"
               "[].forEach.call(document.querySelectorAll('.rack'),function(r){"
               "var v=x[r.parentNode.className];if(v)r.scrollLeft=v});"
               "try{window.scrollTo(X,Y)}catch(e){}}\n"
               /* One request at a time, no answer in 4 s is "no
                * connection" (that request's answer, late, is dropped);
                * an answer to an older request is dropped;
                * the same page as before changes nothing (a selection
                * or the focus stays) */
               "var last=null,busy=0,gen=0;\n"
               "function tick(){if(busy||!window.fetch||!window.DOMParser)"
               "return;busy=1;var g=++gen,k=window.AbortController?"
               "new AbortController():null,w=setTimeout(function(){"
               "if(k)k.abort();if(g===gen){gen++;busy=0;stale(true)}},4000),"
               "o={cache:'no-store',credentials:'same-origin'};"
               "if(k)o.signal=k.signal;"
               "fetch(location.href,o).then(function(r){if(!r.ok)throw r.status;"
               "return r.text()}).then(function(h){if(g!==gen)return;"
               "if(h===last){stale(false);return}var d=new DOMParser()"
               ".parseFromString(h,'text/html'),m=d.querySelector("
               "'meta[name=camex-version]');if(!m||m.content!==V){"
               "location.reload();return}last=h;swap(d);stale(false)})"
               "['catch'](function(){if(g===gen)stale(true)})"
               ".then(function(){clearTimeout(w);if(g===gen)busy=0})}\n"
               "setInterval(tick,5000);\n"
               "})();\n"
               "</script>\n</body>\n</html>\n");
    free(conf);
    free(autos);
    if (out.err) {
        free(out.p);
        return NULL;
    }
    *count = (size_t)live;
    return out.p;
}
