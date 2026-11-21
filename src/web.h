/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * web.h — the start page and nginx's lookup served by the server itself
 * (#203, docs/nginx.md): web_listen= in [server]
 *
 *   GET /             the start page (nginxmap.c), every server on this
 *                     host unless web_scope = self
 *   GET /status.json  {"servers":[...]}, as camex --status --json
 *   GET /resolve      for nginx's auth_request: 204 with X-Camex-Upstream:
 *                     <address>:<port> of the device X-Camex-Host names
 *                     (empty: no such device); from 127.0.0.1 only
 *
 * Read only: nothing here changes the server. Without CAMEX_WEB (a
 * client-only build, #204) the functions do nothing.
 *
 */

#ifndef CAMEX_WEB_H
#define CAMEX_WEB_H

#if !defined(CAMEX_NO_WEB) && !defined(_WIN32)

#include <sys/select.h>

/* web_listen=, web_password=, web_scope= of server_db.globals in effect:
 * opens, moves or closes the listener; 0, or -1 when it cannot run (a
 * port taken, a LAN address without web_password=): the server stops */
int web_apply(void);

void web_close(void);

int web_fdset(fd_set *readset, fd_set *writeset, int maxfd);

void web_handle(const fd_set *readset, const fd_set *writeset);

#else

static inline int web_apply(void)
{
    return 0;
}

static inline void web_close(void)
{
}

#endif

#endif /* CAMEX_WEB_H */
