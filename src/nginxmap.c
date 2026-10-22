/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * nginxmap.c — an nginx map of the live clients (#84)
 *
 */

#include "nginxmap.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define MAP_HOST_MAX 254   /* a host name, without its NUL */

typedef struct {
    char host[MAP_HOST_MAX + 1];
    char addr[32];   /* a dotted IPv4 address (valid_addr()) */
} map_entry_t;

/* A host label: 1-63 of a-z, 0-9 and '-', not starting or ending with '-';
 * label is in lower case already */
static int valid_label(const char *label)
{
    size_t n = strlen(label), i;

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

/* A domain: labels separated by dots */
static int valid_domain(const char *domain)
{
    char buf[MAP_HOST_MAX + 1];
    char *label, *save = NULL;
    size_t i, n = strlen(domain);

    if (n == 0U || n >= sizeof(buf) || domain[n - 1U] == '.' ||
        domain[0] == '.') {
        return 0;
    }
    for (i = 0U; i <= n; ++i) {
        buf[i] = (char)tolower((unsigned char)domain[i]);
    }
    for (label = strtok_r(buf, ".", &save); label != NULL;
         label = strtok_r(NULL, ".", &save)) {
        if (!valid_label(label)) {
            return 0;
        }
    }
    return strstr(domain, "..") == NULL;
}

/* host:port or a plain IPv4 address with a port: what nginx takes as a
 * proxy target; letters, digits, '.', '-', ':', '[', ']' */
static int valid_target(const char *t)
{
    size_t i, n = strlen(t);

    if (n == 0U || n > 128U) {
        return 0;
    }
    for (i = 0U; i < n; ++i) {
        unsigned char c = (unsigned char)t[i];

        if (!(isalnum(c) || c == '.' || c == '-' || c == ':' || c == '[' ||
              c == ']')) {
            return 0;
        }
    }
    return 1;
}

static int valid_addr(const char *a)
{
    unsigned int b[4];
    char tail;

    return sscanf(a, "%u.%u.%u.%u%c", &b[0], &b[1], &b[2], &b[3],
                  &tail) == 4 &&
           b[0] < 256U && b[1] < 256U && b[2] < 256U && b[3] < 256U;
}

/* By host, then by address: the same ID twice keeps the lower address,
 * whatever order the servers answered in */
static int cmp_entry(const void *a, const void *b)
{
    const map_entry_t *x = a, *y = b;
    int c = strcmp(x->host, y->host);

    return c != 0 ? c : strcmp(x->addr, y->addr);
}

char *nginx_map_format(const char *lines, const char *domain, int port,
                       const char *dflt, FILE *warn, size_t *count)
{
    map_entry_t *e = NULL;
    size_t n = 0U, cap = 0U, i, out_cap, len = 0U;
    const char *p = lines != NULL ? lines : "";
    char *out;

    *count = 0U;
    if ((domain != NULL && domain[0] != '\0' && !valid_domain(domain)) ||
        port < 1 || port > 65535 ||
        (dflt != NULL && dflt[0] != '\0' && !valid_target(dflt))) {
        return NULL;
    }
    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        size_t ll = eol != NULL ? (size_t)(eol - p) : strlen(p);
        char line[128], id[64], addr[32], label[64];
        size_t k;

        if (ll > 0U && ll < sizeof(line)) {
            memcpy(line, p, ll);
            line[ll] = '\0';
            if (sscanf(line, "%63s %31s", id, addr) == 2 &&
                valid_addr(addr)) {
                for (k = 0U; id[k] != '\0'; ++k) {
                    label[k] = (char)tolower((unsigned char)id[k]);
                }
                label[k] = '\0';
                if (!valid_label(label)) {
                    if (warn != NULL) {
                        fprintf(warn, "camex: client ID %s is not a host "
                                "name label: left out of the map\n", id);
                    }
                } else {
                    map_entry_t *grown;

                    if (n == cap) {
                        cap = cap == 0U ? 64U : cap * 2U;
                        grown = realloc(e, cap * sizeof(*e));
                        if (grown == NULL) {
                            free(e);
                            return NULL;
                        }
                        e = grown;
                    }
                    if (domain != NULL && domain[0] != '\0') {
                        snprintf(e[n].host, sizeof(e[n].host), "%s.%s",
                                 label, domain);
                    } else {
                        snprintf(e[n].host, sizeof(e[n].host), "%s", label);
                    }
                    for (k = 0U; e[n].host[k] != '\0'; ++k) {
                        e[n].host[k] = (char)tolower(
                            (unsigned char)e[n].host[k]);
                    }
                    snprintf(e[n].addr, sizeof(e[n].addr), "%s", addr);
                    n++;
                }
            }
        }
        p += ll + (eol != NULL ? 1U : 0U);
    }
    if (n > 1U) {
        qsort(e, n, sizeof(*e), cmp_entry);
    }

    out_cap = 256U + (dflt != NULL ? strlen(dflt) : 0U) +
              n * (MAP_HOST_MAX + 40U);
    out = malloc(out_cap);
    if (out == NULL) {
        free(e);
        return NULL;
    }
    len += (size_t)snprintf(out + len, out_cap - len,
                            "# camex --status --format nginx-map: generated, "
                            "do not edit\nmap $host $camex_upstream {\n");
    if (dflt != NULL && dflt[0] != '\0') {
        len += (size_t)snprintf(out + len, out_cap - len,
                                "    default %s;\n", dflt);
    }
    for (i = 0U; i < n; ++i) {
        /* The same ID on two servers: the lower address is kept */
        if (i > 0U && strcmp(e[i].host, e[i - 1U].host) == 0) {
            if (warn != NULL) {
                fprintf(warn, "camex: %s is listed twice (two servers?): "
                        "only %s kept\n", e[i].host, e[i - 1U].addr);
            }
            continue;
        }
        len += (size_t)snprintf(out + len, out_cap - len, "    %s %s:%d;\n",
                                e[i].host, e[i].addr, port);
        (*count)++;
    }
    snprintf(out + len, out_cap - len, "}\n");
    free(e);
    return out;
}
