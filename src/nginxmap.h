/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * nginxmap.h — an nginx map of the live clients (#84):
 * camex --status --format nginx-map
 *
 */

#ifndef CAMEX_NGINXMAP_H
#define CAMEX_NGINXMAP_H

#include <stddef.h>
#include <stdio.h>

/* Build the map from "<client ID> <tunnel address>" lines (the servers'
 * CLIENTS answers, one after the other):
 *
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

#endif /* CAMEX_NGINXMAP_H */
