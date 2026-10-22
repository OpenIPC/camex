/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * nginxmap.c — an nginx map of the live clients (#84)
 *
 */

#include "nginxmap.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAP_HOST_MAX 253   /* a host name, without its NUL */

typedef struct {
    char host[MAP_HOST_MAX + 1];
    uint32_t addr;         /* host order, for the order of duplicates */
    char addr_text[16];
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

/* A dotted IPv4 address: four numbers 0..255, nothing else */
static int parse_addr(const char *a, uint32_t *out)
{
    unsigned int b[4];
    int used = 0;

    if (sscanf(a, "%3u.%3u.%3u.%3u%n", &b[0], &b[1], &b[2], &b[3],
               &used) != 4 || a[used] != '\0' || !isdigit((unsigned char)a[0]) ||
        b[0] > 255U || b[1] > 255U || b[2] > 255U || b[3] > 255U) {
        return 0;
    }
    *out = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
    return 1;
}

/* host:port for --default: a host name or an IPv4 address, a port
 * 1..65535 */
static int valid_target(const char *t)
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
    port = strtol(colon + 1, &end, 10);
    if (colon[1] == '\0' || *end != '\0' || port < 1L || port > 65535L) {
        return 0;
    }
    return parse_addr(host, &a) || valid_hostname(host);
}

static int cmp_entry(const void *a, const void *b)
{
    const map_entry_t *x = a, *y = b;
    int c = strcmp(x->host, y->host);

    /* The same ID twice: the lower address (numerically) first */
    if (c != 0) {
        return c;
    }
    return x->addr < y->addr ? -1 : x->addr > y->addr ? 1 : 0;
}

char *nginx_map_format(const char *lines, const char *domain, int port,
                       const char *dflt, FILE *warn, size_t *count)
{
    map_entry_t *e = NULL;
    size_t n = 0U, cap = 0U, i;
    size_t dlen = domain != NULL ? strlen(domain) : 0U;
    const char *p = lines != NULL ? lines : "";
    map_buf_t out = { NULL, 0U, 0U, 0 };

    *count = 0U;
    if ((dlen > 0U && !valid_hostname(domain)) || port < 1 || port > 65535 ||
        (dflt != NULL && dflt[0] != '\0' && !valid_target(dflt))) {
        return NULL;
    }
    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        size_t ll = eol != NULL ? (size_t)(eol - p) : strlen(p);
        char line[128], id[64], addr[32], extra[2], label[64];
        size_t k;
        uint32_t a;

        /* A record is exactly "<ID> <address>": anything else is not used */
        if (ll > 0U && ll < sizeof(line)) {
            memcpy(line, p, ll);
            line[ll] = '\0';
            if (sscanf(line, "%63s %31s %1s", id, addr, extra) == 2 &&
                parse_addr(addr, &a)) {
                for (k = 0U; id[k] != '\0'; ++k) {
                    label[k] = (char)tolower((unsigned char)id[k]);
                }
                label[k] = '\0';
                if (!valid_label(label, k)) {
                    if (warn != NULL) {
                        fprintf(warn, "camex: client ID %s is not a host "
                                "name label: left out of the map\n", id);
                    }
                } else if (dlen > 0U && k + 1U + dlen > MAP_HOST_MAX) {
                    if (warn != NULL) {
                        fprintf(warn, "camex: %s.%s is longer than a host "
                                "name: left out of the map\n", label, domain);
                    }
                } else {
                    if (n == cap) {
                        map_entry_t *grown;

                        cap = cap == 0U ? 64U : cap * 2U;
                        grown = realloc(e, cap * sizeof(*e));
                        if (grown == NULL) {
                            free(e);
                            return NULL;
                        }
                        e = grown;
                    }
                    if (dlen > 0U) {
                        snprintf(e[n].host, sizeof(e[n].host), "%s.%s",
                                 label, domain);
                    } else {
                        snprintf(e[n].host, sizeof(e[n].host), "%s", label);
                    }
                    for (k = 0U; e[n].host[k] != '\0'; ++k) {
                        e[n].host[k] = (char)tolower(
                            (unsigned char)e[n].host[k]);
                    }
                    e[n].addr = a;
                    snprintf(e[n].addr_text, sizeof(e[n].addr_text),
                             "%u.%u.%u.%u", (unsigned)(a >> 24),
                             (unsigned)((a >> 16) & 255U),
                             (unsigned)((a >> 8) & 255U),
                             (unsigned)(a & 255U));
                    n++;
                }
            }
        }
        p += ll + (eol != NULL ? 1U : 0U);
    }
    if (n > 1U) {
        qsort(e, n, sizeof(*e), cmp_entry);
    }

    buf_printf(&out, "# camex --status --format nginx-map: generated, do "
               "not edit\nmap $host $camex_upstream {\n");
    if (dflt != NULL && dflt[0] != '\0') {
        buf_printf(&out, "    default %s;\n", dflt);
    }
    for (i = 0U; i < n; ++i) {
        /* The same ID on two servers: the lower address is kept */
        if (i > 0U && strcmp(e[i].host, e[i - 1U].host) == 0) {
            if (warn != NULL) {
                fprintf(warn, "camex: %s is listed twice (two servers?): "
                        "only %s kept\n", e[i].host, e[i - 1U].addr_text);
            }
            continue;
        }
        /* Quoted: an ID such as "default" or "include" stays a host */
        buf_printf(&out, "    \"%s\" %s:%d;\n", e[i].host, e[i].addr_text,
                   port);
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
