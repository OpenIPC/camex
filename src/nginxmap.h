/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * nginxmap.h — an nginx map of the live clients (#84):
 * camex --status --format nginx-map; and their start page (#105):
 * camex --status --format nginx-index
 *
 */

#ifndef CAMEX_NGINXMAP_H
#define CAMEX_NGINXMAP_H

#include <stddef.h>
#include <stdio.h>

/* Build the map from "<client ID> <tunnel address>" lines (the servers'
 * CLIENTS answers, one after the other):
 *
 *   # domain: <domain> | (none)              (a comment, #106)
 *   map $host $camex_upstream {
 *       default <dflt>;                      (only with dflt)
 *       <id in lower case>[.<domain>] <address>:<port>;
 *   }
 *
 * sorted by host, so an unchanged state gives an identical file. IDs that
 * are not a valid host label are left out, and of an ID seen twice (on
 * two servers) the lower address is kept, each with a warning on warn (NULL: none). Returns the map (malloc'd, NUL-terminated)
 * and the number of clients in it in *count, or NULL without memory or
 * for a bad domain, port or default. */
char *nginx_map_format(const char *lines, const char *domain, int port,
                       const char *dflt, FILE *warn, size_t *count);

/* A domain for the map and the page (--domain, domain= in [server],
 * #106): host name labels separated by single dots, up to 253 characters.
 * 1 if valid */
int camex_domain_valid(const char *domain);

/* The longest display name (name= in a [client ID] block, #105), in bytes */
#define CAMEX_NAME_MAX 64

/* A display name: 1..CAMEX_NAME_MAX bytes of UTF-8 (no overlong forms,
 * surrogates or code points above U+10FFFF) without control characters
 * (C0, DEL, C1). 1 if valid. */
int camex_name_valid(const char *name);

/* Build the start page from "<client ID> <tunnel address>[ <name>]" lines
 * (the servers' CLIENTS NAMES answers): a static HTML page with one row
 * per client, its name (the ID without one) linked to
 * //<id in lower case>[.<domain>]/, so that it works over HTTP and HTTPS.
 * IDs are checked as for the map, of an ID seen twice the lower address
 * is kept, and a name that is not valid is shown as the ID. Rows are
 * sorted by name, then by ID, and the page has no time in it: an
 * unchanged state gives an identical file. Returns the page (malloc'd,
 * NUL-terminated) and the number of clients in it in *count, or NULL
 * without memory or for a bad domain. */
char *nginx_index_format(const char *lines, const char *domain, FILE *warn,
                         size_t *count);

#endif /* CAMEX_NGINXMAP_H */
