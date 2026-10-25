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
    char name[CAMEX_NAME_MAX + 1];   /* "" = none (the start page) */
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

/* One "<ID> <address>[ <name>]" record (with_name: the name may follow
 * after one blank; without, a record is exactly two fields) into *e.
 * 1 if it is a client to list; 0 for anything else, with a warning on
 * warn for an ID that cannot be a host name */
static int parse_record(const char *p, size_t ll, const char *domain,
                        size_t dlen, int with_name, FILE *warn,
                        map_entry_t *e)
{
    char line[256], id[64], addr[32], extra[2], shown[64];
    int used = 0;
    size_t k;
    uint32_t a;

    if (ll == 0U || ll >= sizeof(line)) {
        return 0;
    }
    memcpy(line, p, ll);
    line[ll] = '\0';
    if (with_name) {
        if (sscanf(line, "%63s %31s%n", id, addr, &used) != 2) {
            return 0;
        }
    } else if (sscanf(line, "%63s %31s %1s", id, addr, extra) != 2) {
        return 0;
    }
    if (!parse_addr(addr, &a)) {
        return 0;
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
                    with_name ? "start page" : "map");
        }
        return 0;
    }
    if (dlen > 0U && k + 1U + dlen > MAP_HOST_MAX) {
        if (warn != NULL) {
            fprintf(warn, "camex: %s.%s is longer than a host name: left "
                    "out of the %s\n", e->label, domain,
                    with_name ? "start page" : "map");
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
    snprintf(e->addr_text, sizeof(e->addr_text), "%u.%u.%u.%u",
             (unsigned)(a >> 24), (unsigned)((a >> 16) & 255U),
             (unsigned)((a >> 8) & 255U), (unsigned)(a & 255U));
    e->name[0] = '\0';
    /* The name: everything after the one blank that follows the address.
     * The server checked it when it loaded its file; one that is not
     * valid now (another program on the socket) is shown as the ID */
    if (with_name && line[used] == ' ' && line[used + 1] != '\0') {
        if (camex_name_valid(line + used + 1)) {
            snprintf(e->name, sizeof(e->name), "%s", line + used + 1);
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
                          int with_name, FILE *warn, map_entry_t **out)
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
        if (parse_record(p, ll, domain, dlen, with_name, warn, &e[n])) {
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
               "not edit\nmap $host $camex_upstream {\n");
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
 * the order does not depend on the servers' order or the locale */
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
    if (c != 0) {
        return c;
    }
    c = strcmp(shown_name(x), shown_name(y));
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

char *nginx_index_format(const char *lines, const char *domain, FILE *warn,
                         size_t *count)
{
    map_entry_t *e = NULL;
    size_t dlen = domain != NULL ? strlen(domain) : 0U;
    map_buf_t out = { NULL, 0U, 0U, 0 };
    long n, i, kept = 0;

    *count = 0U;
    if (dlen > 0U && !valid_hostname(domain)) {
        return NULL;
    }
    n = parse_records(lines, domain, 1, warn, &e);
    if (n < 0) {
        return NULL;
    }
    /* The same ID on two servers: the lower address is kept, as in the
     * map, then the rows go by name */
    for (i = 0; i < n; ++i) {
        if (i > 0 && strcmp(e[i].host, e[kept - 1].host) == 0) {
            if (warn != NULL) {
                fprintf(warn, "camex: %s is listed twice (two servers?): "
                        "only %s kept\n", e[i].host, e[kept - 1].addr_text);
            }
            continue;
        }
        e[kept++] = e[i];
    }
    if (kept > 1) {
        qsort(e, (size_t)kept, sizeof(*e), cmp_shown);
    }

    /* The look of OpenIPC/vectis's page: brass, rivets and lamps */
    buf_printf(&out,
        "<!doctype html>\n"
        "<!-- camex --status --format nginx-index: generated, do not edit "
        "-->\n"
        "<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width,"
        "initial-scale=1\">\n"
        "<meta http-equiv=\"refresh\" content=\"30\">\n"
        "<title>Camex Devices</title>\n"
        "<style>\n"
        ":root{--panel:#2c2119;--panel2:#3d2d22;--brass:#c9a24d;"
        "--brass2:#8c6a2a;--amber:#ffb347;--ink:#e8d9b8;--dim:#8a7455;"
        "--green:#5ad38f}\n"
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
        ".panel{position:relative;width:min(760px,100%%);"
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
        ".rack{margin-top:20px;border:1px solid #574123;border-radius:10px;"
        "background:rgba(0,0,0,.25);box-shadow:inset 0 2px 8px rgba(0,0,0,.6);"
        "overflow:hidden}\n"
        "table{width:100%%;border-collapse:collapse}\n"
        "th{font-family:'Courier New',monospace;font-size:11px;"
        "font-weight:normal;letter-spacing:.2em;text-transform:uppercase;"
        "color:var(--dim);text-align:left;padding:10px 12px;"
        "border-bottom:1px solid #574123}\n"
        "td{padding:12px;border-bottom:1px solid rgba(87,65,35,.5);"
        "vertical-align:middle}\n"
        "tr:last-child td{border-bottom:0}\n"
        "tbody tr:hover{background:rgba(201,162,77,.07)}\n"
        ".bulb{display:block;width:14px;height:14px;border-radius:50%%;"
        "border:2px solid #6e5320;background:radial-gradient(circle at 40%% "
        "35%%,#d8ffe8,var(--green) 45%%,#155b34);box-shadow:0 0 10px "
        "var(--green)}\n"
        "td.lamp{width:38px}\n"
        "a{color:var(--amber);text-decoration:none;font-size:17px;"
        "letter-spacing:.04em;text-shadow:0 0 10px rgba(255,179,71,.35);"
        "word-break:break-word}\n"
        "a:hover{text-decoration:underline}\n"
        "td.id,td.addr{font-family:'Courier New',monospace;font-size:13px;"
        "color:var(--dim);letter-spacing:.06em;white-space:nowrap}\n"
        ".empty{margin-top:20px;font-family:'Courier New',monospace;"
        "font-size:clamp(16px,3vw,22px);letter-spacing:.18em;"
        "text-transform:uppercase;color:var(--dim);padding:14px 16px;"
        "border:2px solid #574123;border-radius:8px;background:#120d0a;"
        "box-shadow:inset 0 0 20px rgba(0,0,0,.8);text-align:center}\n"
        ".foot{margin-top:18px;font-family:'Courier New',monospace;"
        "font-size:11px;color:#5e4d36;letter-spacing:.2em;"
        "text-transform:uppercase;text-align:center}\n"
        "@media (max-width:560px){.addr{display:none}.panel{padding:26px "
        "14px 18px}td,th{padding:10px 8px}}\n"
        "</style>\n</head>\n<body>\n<h1>Camex Devices</h1>\n");
    buf_printf(&out, "<div class=\"sub\">%ld %s online</div>\n"
               "<div class=\"panel\">\n"
               "<span class=\"rivet tl\"></span><span class=\"rivet tr\">"
               "</span><span class=\"rivet bl\"></span><span class=\"rivet "
               "br\"></span>\n"
               "<div class=\"top\"><span class=\"plate\">Device registry"
               "</span><span class=\"plate\">camex %s</span></div>\n",
               kept, kept == 1 ? "device" : "devices", CAMEX_VERSION);
    if (kept == 0) {
        buf_printf(&out, "<div class=\"empty\">No devices online</div>\n");
    } else {
        buf_printf(&out, "<div class=\"rack\">\n<table>\n<thead><tr><th>"
                   "</th><th>Name</th><th>ID</th><th class=\"addr\">Address"
                   "</th></tr></thead>\n<tbody>\n");
        for (i = 0; i < kept; ++i) {
            /* The host and the ID are host name labels: nothing to
             * escape in them */
            buf_printf(&out, "<tr class=\"dev\"><td class=\"lamp\"><span "
                       "class=\"bulb\"></span></td><td><a href=\"//%s/\">",
                       e[i].host);
            buf_html(&out, shown_name(&e[i]));
            buf_printf(&out, "</a></td><td class=\"id\">%s</td><td "
                       "class=\"addr\">%s</td></tr>\n", e[i].label,
                       e[i].addr_text);
        }
        buf_printf(&out, "</tbody>\n</table>\n</div>\n");
    }
    buf_printf(&out, "</div>\n<div class=\"foot\">Camex &middot; OpenIPC "
               "&middot; MIT License</div>\n</body>\n</html>\n");
    free(e);
    if (out.err) {
        free(out.p);
        return NULL;
    }
    *count = (size_t)kept;
    return out.p;
}
