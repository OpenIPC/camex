/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * control.h — the server's local control socket (#82): camex --status
 *
 * The server listens on a Unix domain socket (mode 0600, so only its own
 * user, normally root, can connect) and answers one command a connection:
 *
 *   STATUS        a table of the live clients, for people
 *   STATUS JSON   the same as one JSON object, for scripts
 *   WAIT <h> <s>  the clients' state fingerprint once it is not <h>, or
 *                 after <s> seconds (#130)
 *
 * By default the socket is CAMEX_CONTROL_DIR/camex-<udp|tcp>-<port>.sock,
 * so a UDP and a TCP server on one host each have their own;
 * control_socket= in [server] or --control <path> changes it, "none"
 * turns it off. camex --status without --control asks every socket in
 * CAMEX_CONTROL_DIR. Not on Windows.
 *
 */

#ifndef CAMEX_CONTROL_H
#define CAMEX_CONTROL_H

#include <stddef.h>

#ifndef _WIN32
#include <sys/select.h>
#endif

#ifdef __linux__
#define CAMEX_CONTROL_DIR "/run/camex"
#else
#define CAMEX_CONTROL_DIR "/var/run/camex"
#endif

/* Default socket path of a server with this transport (0 UDP, 1 TCP) and
 * port */
void control_default_path(char *buf, size_t size, int transport, int port);

#ifndef _WIN32
/* Server: listen on path ("" = the default for transport and port,
 * "none" = off). 0, or -1 (logged): the server runs on without it. */
int control_open(const char *path, int transport, int port);

/* Server, after a config reload: listen on path if it differs from the
 * one asked for before (control_socket= changed); same return values */
int control_reopen(const char *path, int transport, int port);

/* Server: stop listening and remove the socket */
void control_close(void);

/* Server loop: add the control descriptors to the sets; returns maxfd */
int control_fdset(fd_set *readset, fd_set *writeset, int maxfd);

/* Server loop: serve the ready control descriptors */
void control_handle(const fd_set *readset, const fd_set *writeset);
#endif

/* camex --status [--json] [--control <path>]: print the status of one
 * server, or of every server in CAMEX_CONTROL_DIR. Exit code: 0, or 1. */
int control_status(const char *path, int json);

/* camex --status --format nginx-map (#84): the clients of one server, or
 * of every server in CAMEX_CONTROL_DIR, as an nginx map (nginxmap.h).
 * Exit code 1, and no map, when a server does not answer. */
int control_nginx_map(const char *path, const char *domain, int port,
                      const char *dflt);

/* camex --status --format nginx-index (#105): the same clients, with
 * their name= from the server's file, as a start page (nginxmap.h).
 * Exit code 1, and no page, when a server does not answer. */
int control_nginx_index(const char *path, const char *domain);

/* camex --status --wait <token> (#130): wait until the state of the
 * clients changes on any server (or the one at path), at most timeout
 * seconds, then print the token of the state now. The token is opaque
 * ("<hash>:<socket>,..."); "-" asks for the state at once. Exit code 0
 * (changed or timed out), 3 when a server is too old for it, 1 on an
 * error. */
int control_wait(const char *path, const char *token, int timeout);

#endif /* CAMEX_CONTROL_H */
