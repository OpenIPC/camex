/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * control.c — the server's local control socket (#82): camex --status
 *
 * The server never blocks on it: connections are non-blocking, a command
 * is one short line, the answer is built at once and sent as the socket
 * takes it, and a connection that takes longer than CONTROL_TIMEOUT
 * seconds is closed.
 *
 */

#include "control.h"
#include "camex.h"
#include "log.h"
#include "server.h"
#include "nginxmap.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#define CONTROL_CONNS   4
#define CONTROL_TIMEOUT 10
#define CONTROL_CMD_MAX 64
#define CONTROL_END     ".\n"   /* last line of every answer */

void control_default_path(char *buf, size_t size, int transport, int port)
{
    const char *proto = transport == CAMEX_TRANSPORT_TCP ? "tcp" : "udp";
    const char *ip = current_config.bind_ip;
    const char *dev = current_config.bind_dev;

    /* Servers on one port bound to other addresses or interfaces each get
     * their own socket */
    if (ip[0] != '\0' && strcmp(ip, "0.0.0.0") != 0) {
        snprintf(buf, size, "%s/camex-%s-%s-%d.sock", CAMEX_CONTROL_DIR,
                 proto, ip, port);
    } else if (dev[0] != '\0' && strchr(dev, '/') == NULL) {
        snprintf(buf, size, "%s/camex-%s-%s-%d.sock", CAMEX_CONTROL_DIR,
                 proto, dev, port);
    } else {
        snprintf(buf, size, "%s/camex-%s-%d.sock", CAMEX_CONTROL_DIR, proto,
                 port);
    }
}

#ifndef _WIN32

typedef struct {
    int fd;                 /* -1 = unused */
    char cmd[CONTROL_CMD_MAX];
    size_t cmd_len;
    char *out;              /* the answer, once the command is read */
    size_t out_len;
    size_t out_off;
    time_t since;
} control_conn_t;

static int control_fd = -1;
static char control_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static char control_spec[sizeof(control_path)];   /* as asked: "", "none" */
static int control_spec_ok = 0;   /* control_spec is in effect */
static time_t control_paused_until = 0;   /* accept() failed: wait */
static control_conn_t control_conns[CONTROL_CONNS] = {
    { -1, "", 0U, NULL, 0U, 0U, 0 }, { -1, "", 0U, NULL, 0U, 0U, 0 },
    { -1, "", 0U, NULL, 0U, 0U, 0 }, { -1, "", 0U, NULL, 0U, 0U, 0 }
};

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* A socket at path that nobody listens on (a server that did not stop
 * cleanly); its inode in *st */
static int stale_socket(const char *path, struct stat *st)
{
    struct sockaddr_un sa;
    int fd, rc;

    if (lstat(path, st) != 0 || !S_ISSOCK(st->st_mode)) {
        return 0;
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return 0;
    }
    /* Non-blocking: a live server with a full backlog must not hold up
     * this one's start; only a refused connection means stale */
    if (set_nonblock(fd) != 0) {
        close(fd);
        return 0;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    rc = rc != 0 && errno == ECONNREFUSED;
    close(fd);
    return rc;
}

/* Listen on path ("" = the default): the new socket's path and
 * descriptor, or -1 (logged) */
static int control_listen(const char *path, int transport, int port,
                          char *newpath, size_t newsize, int *fd_out)
{
    struct sockaddr_un sa;
    char dir[sizeof(control_path)];
    char *slash;
    mode_t old_mask;
    int fd;

    if (path == NULL || path[0] == '\0') {
        control_default_path(newpath, newsize, transport, port);
    } else if (strlen(path) >= newsize) {
        log_message(LOG_WARNING, "Control socket path too long: %s "
                    "(camex --status will not reach this server)", path);
        return -1;
    } else {
        snprintf(newpath, newsize, "%s", path);
    }

    /* Its directory: created when missing (CAMEX_CONTROL_DIR is on a
     * tmpfs, empty after a reboot) */
    snprintf(dir, sizeof(dir), "%s", newpath);
    slash = strrchr(dir, '/');
    if (slash != NULL && slash != dir) {
        *slash = '\0';
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
            log_message(LOG_WARNING, "Cannot create %s: %s (camex --status "
                        "will not reach this server; control_socket= sets "
                        "another path)", dir, strerror(errno));
                        return -1;
        }
    }
    {
        struct stat stale, now;

        /* Only the stale socket that was checked: another server that
         * bound the path in between keeps its own */
        if (stale_socket(newpath, &stale) &&
            lstat(newpath, &now) == 0 && now.st_ino == stale.st_ino &&
            now.st_dev == stale.st_dev) {
            unlink(newpath);
        }
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        log_message(LOG_WARNING, "Control socket: %s", strerror(errno));
                return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", newpath);
    /* 0600 from the start: no moment in which others could connect */
    old_mask = umask(0177);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        int err = errno;

        umask(old_mask);
        close(fd);
        log_message(LOG_WARNING, "Cannot listen on %s: %s%s", newpath,
                    strerror(err), err == EADDRINUSE
                        ? " (another server uses it: set control_socket=)"
                        : "");
                return -1;
    }
    umask(old_mask);
    /* select() watches only descriptors below FD_SETSIZE */
    if (fd >= FD_SETSIZE) {
        log_message(LOG_WARNING, "Control socket %s: descriptor %d is above "
                    "FD_SETSIZE (camex --status will not reach this server)",
                    newpath, fd);
        close(fd);
        unlink(newpath);
                return -1;
    }
    if (listen(fd, CONTROL_CONNS) != 0 || set_nonblock(fd) != 0) {
        log_message(LOG_WARNING, "Control socket %s: %s", newpath,
                    strerror(errno));
        close(fd);
        unlink(newpath);
                return -1;
    }
    *fd_out = fd;
    log_message(LOG_INFO, "Control socket: %s (camex --status)",
                newpath);
    return 0;
}

int control_open(const char *path, int transport, int port)
{
    char newpath[sizeof(control_path)];
    int fd;

    snprintf(control_spec, sizeof(control_spec), "%s",
             path != NULL ? path : "");
    control_spec_ok = 0;
    if (path != NULL && strcmp(path, "none") == 0) {
        control_spec_ok = 1;
        return 0;
    }
    if (control_listen(path, transport, port, newpath, sizeof(newpath),
                       &fd) != 0) {
        return -1;   /* not recorded: a reload with this path tries again */
    }
    control_fd = fd;
    memcpy(control_path, newpath, sizeof(control_path));
    control_spec_ok = 1;
    return 0;
}

static void control_conn_close(control_conn_t *c)
{
    if (c->fd >= 0) {
        close(c->fd);
    }
    free(c->out);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

void control_close(void)
{
    size_t i;

    for (i = 0U; i < CONTROL_CONNS; ++i) {
        control_conn_close(&control_conns[i]);
    }
    if (control_fd >= 0) {
        close(control_fd);
        control_fd = -1;
        unlink(control_path);
    }
    control_path[0] = '\0';
}

int control_reopen(const char *path, int transport, int port)
{
    const char *want = path != NULL ? path : "";
    char newpath[sizeof(control_path)];
    int fd;

    if (control_spec_ok && strcmp(control_spec, want) == 0) {
        return 0;   /* unchanged: the listener stays */
    }
    if (strcmp(want, "none") == 0) {
        control_close();
        snprintf(control_spec, sizeof(control_spec), "%s", want);
        control_spec_ok = 1;
        log_message(LOG_INFO, "Control socket off (control_socket=none)");
        return 0;
    }
    /* The new socket first: if it cannot be had, the old one stays and the
     * next reload tries again */
    if (control_listen(path, transport, port, newpath, sizeof(newpath),
                       &fd) != 0) {
        if (control_fd >= 0) {
            log_message(LOG_WARNING, "Control socket stays at %s",
                        control_path);
        }
        return -1;
    }
    control_close();
    control_fd = fd;
    memcpy(control_path, newpath, sizeof(control_path));
    snprintf(control_spec, sizeof(control_spec), "%s", want);
    control_spec_ok = 1;
    return 0;
}

int control_fdset(fd_set *readset, fd_set *writeset, int maxfd)
{
    size_t i;

    if (control_fd < 0) {
        return maxfd;
    }
    if (time(NULL) >= control_paused_until) {
        FD_SET(control_fd, readset);
        if (control_fd > maxfd) {
            maxfd = control_fd;
        }
    }
    for (i = 0U; i < CONTROL_CONNS; ++i) {
        control_conn_t *c = &control_conns[i];

        if (c->fd < 0) {
            continue;
        }
        if (difftime(time(NULL), c->since) > (double)CONTROL_TIMEOUT) {
            control_conn_close(c);
            continue;
        }
        FD_SET(c->fd, c->out != NULL ? writeset : readset);
        if (c->fd > maxfd) {
            maxfd = c->fd;
        }
    }
    return maxfd;
}

/* The command is in: build the answer */
static void control_answer(control_conn_t *c)
{
    char *nl = memchr(c->cmd, '\n', c->cmd_len);
    size_t len = 0U;

    if (nl != NULL) {
        *nl = '\0';
    } else {
        c->cmd[c->cmd_len < sizeof(c->cmd) ? c->cmd_len
                                           : sizeof(c->cmd) - 1U] = '\0';
    }
    if (nl != NULL && nl > c->cmd && nl[-1] == '\r') {
        nl[-1] = '\0';
    }
    if (strcmp(c->cmd, "STATUS") == 0) {
        c->out = server_status(0, &len);
    } else if (strcmp(c->cmd, "STATUS JSON") == 0) {
        c->out = server_status(1, &len);
    } else if (strcmp(c->cmd, "CLIENTS") == 0) {
        c->out = server_clients_list(0, &len);
    } else if (strcmp(c->cmd, "CLIENTS NAMES") == 0) {
        c->out = server_clients_list(1, &len);
    } else {
        static const char err[] = "ERROR unknown command (STATUS, "
                                  "STATUS JSON, CLIENTS, CLIENTS NAMES)\n";

        c->out = malloc(sizeof(err));
        if (c->out != NULL) {
            memcpy(c->out, err, sizeof(err));
            len = sizeof(err) - 1U;
        }
    }
    if (c->out != NULL) {
        /* The end marker: the client tells a whole answer from one that
         * was cut off */
        char *grown = realloc(c->out, len + sizeof(CONTROL_END));

        if (grown == NULL) {
            free(c->out);
            c->out = NULL;
        } else {
            c->out = grown;
            memcpy(c->out + len, CONTROL_END, sizeof(CONTROL_END));
            len += sizeof(CONTROL_END) - 1U;
        }
    }
    if (c->out == NULL) {
        control_conn_close(c);
        return;
    }
    c->out_len = len;
    c->out_off = 0U;
}

void control_handle(const fd_set *readset, const fd_set *writeset)
{
    size_t i;

    if (control_fd < 0) {
        return;
    }
    if (FD_ISSET(control_fd, readset)) {
        for (;;) {
            int fd = accept(control_fd, NULL, NULL);
            control_conn_t *c = NULL;

            if (fd < 0) {
                if (errno == EINTR || errno == ECONNABORTED) {
                    continue;
                }
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    /* Out of descriptors, say: a pending connection would
                     * keep the socket readable, so wait a second */
                    control_paused_until = time(NULL) + 1;
                }
                break;
            }
            for (i = 0U; i < CONTROL_CONNS; ++i) {
                if (control_conns[i].fd < 0) {
                    c = &control_conns[i];
                    break;
                }
            }
            /* select() watches only descriptors below FD_SETSIZE */
#ifdef SO_NOSIGPIPE
            {
                /* No MSG_NOSIGNAL (macOS): a client that left must not
                 * stop the server with SIGPIPE */
                int one = 1;

                setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
            }
#endif
            if (c == NULL || fd >= FD_SETSIZE || set_nonblock(fd) != 0) {
                close(fd);
                continue;
            }
            memset(c, 0, sizeof(*c));
            c->fd = fd;
            c->since = time(NULL);
        }
    }
    for (i = 0U; i < CONTROL_CONNS; ++i) {
        control_conn_t *c = &control_conns[i];

        if (c->fd < 0) {
            continue;
        }
        if (c->out == NULL && FD_ISSET(c->fd, readset)) {
            ssize_t n = recv(c->fd, c->cmd + c->cmd_len,
                             sizeof(c->cmd) - 1U - c->cmd_len, 0);

            if (n <= 0) {
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                              errno == EINTR)) {
                    continue;
                }
                control_conn_close(c);
                continue;
            }
            c->cmd_len += (size_t)n;
            if (memchr(c->cmd, '\n', c->cmd_len) != NULL ||
                c->cmd_len >= sizeof(c->cmd) - 1U) {
                control_answer(c);
            }
        } else if (c->out != NULL && FD_ISSET(c->fd, writeset)) {
            ssize_t n;
#ifdef MSG_NOSIGNAL
            n = send(c->fd, c->out + c->out_off, c->out_len - c->out_off,
                     MSG_NOSIGNAL);
#else
            n = send(c->fd, c->out + c->out_off, c->out_len - c->out_off, 0);
#endif
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK ||
                    errno == EINTR) {
                    continue;
                }
                control_conn_close(c);
                continue;
            }
            c->out_off += (size_t)n;
            if (c->out_off >= c->out_len) {
                control_conn_close(c);
            }
        }
    }
}

/* Client side: send cmd to path; the whole answer, without its end
 * marker, in *buf (malloc'd, NUL-terminated). 0, or -1 with a message on
 * stderr (no answer, a cut-off one, an error). */
static int control_ask(const char *path, const char *cmd, char **buf,
                       size_t *buf_len)
{
    struct sockaddr_un sa;
    char chunk[4096];
    ssize_t n;
    int fd;

    if (strlen(path) >= sizeof(sa.sun_path)) {
        fprintf(stderr, "camex: control socket path too long: %s\n", path);
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "camex: socket: %s\n", strerror(errno));
        return -1;
    }
    /* A server that does not answer (stopped, stuck) must not hold up the
     * others */
    {
        struct timeval tv = { 5, 0 };
#ifdef SO_NOSIGPIPE
        int one = 1;

        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        int err = errno;

        fprintf(stderr, "camex: cannot connect to %s: %s%s\n", path,
                strerror(err), err == EACCES
                    ? " (run it as the server's user, normally root)"
                    : (err == ECONNREFUSED || err == ENOENT)
                          ? " (is the server running?)" : "");
        close(fd);
        /* -2: nobody listens there (a server that is gone) */
        return (err == ECONNREFUSED || err == ENOENT) ? -2 : -1;
    }
#ifdef MSG_NOSIGNAL
    if (send(fd, cmd, strlen(cmd), MSG_NOSIGNAL) != (ssize_t)strlen(cmd)) {
#else
    if (send(fd, cmd, strlen(cmd), 0) != (ssize_t)strlen(cmd)) {
#endif
        fprintf(stderr, "camex: %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    while ((n = recv(fd, chunk, sizeof(chunk), 0)) > 0) {
        char *grown = realloc(*buf, *buf_len + (size_t)n + 1U);

        if (grown == NULL) {
            close(fd);
            return -1;
        }
        *buf = grown;
        memcpy(*buf + *buf_len, chunk, (size_t)n);
        *buf_len += (size_t)n;
        (*buf)[*buf_len] = '\0';
    }
    close(fd);
    if (n < 0) {
        fprintf(stderr, "camex: %s: %s\n", path,
                (errno == EAGAIN || errno == EWOULDBLOCK)
                    ? "no answer (timeout)" : strerror(errno));
        return -1;
    }
    /* Whole answers end with the marker line; anything else was cut off
     * (the server busy, stopping, or out of memory) */
    {
        size_t m = sizeof(CONTROL_END) - 1U;

        if (*buf_len == 0U) {
            fprintf(stderr, "camex: %s: no answer (server busy?)\n", path);
            return -1;
        }
        if (*buf_len < m || memcmp(*buf + *buf_len - m, CONTROL_END, m) != 0 ||
            (*buf_len > m && (*buf)[*buf_len - m - 1U] != '\n')) {
            fprintf(stderr, "camex: %s: the answer was cut off\n", path);
            return -1;
        }
        *buf_len -= m;
        (*buf)[*buf_len] = '\0';
        if (strncmp(*buf, "ERROR", 5) == 0) {
            fprintf(stderr, "camex: %s: %s", path, *buf);
            return -1;
        }
    }
    return 0;
}

typedef char control_path_t[108];

/* The socket paths in CAMEX_CONTROL_DIR, sorted, in *out (malloc'd);
 * count, or -1 */
static int control_list(control_path_t **out)
{
    DIR *d = opendir(CAMEX_CONTROL_DIR);
    struct dirent *e;
    control_path_t *paths = NULL;
    int n = 0, cap = 0, i, j;

    *out = NULL;
    if (d == NULL) {
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        struct stat st;

        if (n == cap) {
            control_path_t *grown;

            cap = cap == 0 ? 16 : cap * 2;
            grown = realloc(paths, (size_t)cap * sizeof(*paths));
            if (grown == NULL) {
                free(paths);
                closedir(d);
                return -1;
            }
            paths = grown;
        }

        if (len < 6U || strcmp(e->d_name + len - 5U, ".sock") != 0 ||
            sizeof(CAMEX_CONTROL_DIR) + len + 1U > 108U) {
            continue;
        }
        memcpy(paths[n], CAMEX_CONTROL_DIR, sizeof(CAMEX_CONTROL_DIR) - 1U);
        paths[n][sizeof(CAMEX_CONTROL_DIR) - 1U] = '/';
        memcpy(paths[n] + sizeof(CAMEX_CONTROL_DIR), e->d_name, len + 1U);
        if (stat(paths[n], &st) == 0 && S_ISSOCK(st.st_mode)) {
            n++;
        }
    }
    closedir(d);
    *out = paths;
    for (i = 1; i < n; ++i) {
        for (j = i; j > 0 && strcmp(paths[j - 1], paths[j]) > 0; --j) {
            char t[108];

            memcpy(t, paths[j], sizeof(t));
            memcpy(paths[j], paths[j - 1], sizeof(t));
            memcpy(paths[j - 1], t, sizeof(t));
        }
    }
    return n;
}

int control_status(const char *path, int json)
{
    const char *cmd = json ? "STATUS JSON\n" : "STATUS\n";
    control_path_t one;
    control_path_t *paths = &one;
    control_path_t *listed = NULL;
    int n, i, ok = 0;

    if (path != NULL && path[0] != '\0') {
        n = 1;
        snprintf(one, sizeof(one), "%s", path);
    } else {
        n = control_list(&listed);
        paths = listed;
        if (n <= 0) {
            free(listed);
            fprintf(stderr, "camex: no server control socket in %s (is a "
                    "server running? --control <path> for another one)\n",
                    CAMEX_CONTROL_DIR);
            return 1;
        }
    }
    /* JSON: always {"servers":[...]}, one object per server */
    if (json) {
        fputs("{\"servers\":[", stdout);
    }
    for (i = 0; i < n; ++i) {
        if (json) {
            char *buf = NULL;
            size_t len = 0U;

            if (control_ask(paths[i], cmd, &buf, &len) == 0) {
                while (len > 0U && (buf[len - 1U] == '\n' ||
                                    buf[len - 1U] == '\r')) {
                    buf[--len] = '\0';
                }
                printf("%s%s", ok > 0 ? "," : "", buf);
                ok++;
            }
            free(buf);
        } else {
            if (n > 1) {
                printf("%s== %s\n", i > 0 ? "\n" : "", paths[i]);
            }
            char *buf = NULL;
            size_t len = 0U;

            if (control_ask(paths[i], cmd, &buf, &len) == 0) {
                fwrite(buf, 1U, len, stdout);
                ok++;
            }
            free(buf);
        }
    }
    if (json) {
        fputs("]}\n", stdout);
    }
    fflush(stdout);
    free(listed);
    return ok == n ? 0 : 1;
}

/* cmd to every server (path: only that one): their answers one after
 * the other in *all (malloc'd). 0, or 1 with a message on stderr */
static int control_gather(const char *path, const char *cmd, char **all)
{
    control_path_t one;
    control_path_t *paths = &one;
    control_path_t *listed = NULL;
    size_t all_len = 0U;
    int n, i;

    *all = NULL;
    if (path != NULL && path[0] != '\0') {
        n = 1;
        snprintf(one, sizeof(one), "%s", path);
    } else {
        n = control_list(&listed);
        paths = listed;
        if (n <= 0) {
            free(listed);
            fprintf(stderr, "camex: no server control socket in %s (is a "
                    "server running? --control <path> for another one)\n",
                    CAMEX_CONTROL_DIR);
            return 1;
        }
    }
    /* Every running server must answer: a map without one server's
     * clients would send their names to the offline page, a start page
     * would leave them out */
    for (i = 0; i < n; ++i) {
        char *buf = NULL;
        size_t len = 0U;
        char *grown;

        int rc = control_ask(paths[i], cmd, &buf, &len);

        /* A socket left behind by a server that is gone (found in the
         * directory, not named with --control) has no clients: skip it */
        if (rc == -2 && listed != NULL) {
            fprintf(stderr, "camex: %s: no server listens there, skipped\n",
                    paths[i]);
            free(buf);
            continue;
        }
        if (rc != 0) {
            free(buf);
            free(*all);
            *all = NULL;
            free(listed);
            return 1;
        }
        grown = realloc(*all, all_len + len + 1U);
        if (grown == NULL) {
            free(buf);
            free(*all);
            *all = NULL;
            free(listed);
            return 1;
        }
        *all = grown;
        memcpy(*all + all_len, buf, len);
        all_len += len;
        (*all)[all_len] = '\0';
        free(buf);
    }
    free(listed);
    return 0;
}

int control_nginx_map(const char *path, const char *domain, int port,
                      const char *dflt)
{
    char *all = NULL, *map;
    size_t count = 0U;

    if (control_gather(path, "CLIENTS\n", &all) != 0) {
        return 1;
    }
    map = nginx_map_format(all, domain, port, dflt, stderr, &count);
    free(all);
    if (map == NULL) {
        fprintf(stderr, "camex: cannot build the map (check --domain, "
                "--upstream-port and --default)\n");
        return 1;
    }
    fputs(map, stdout);
    fflush(stdout);
    free(map);
    return 0;
}

int control_nginx_index(const char *path, const char *domain)
{
    char *all = NULL, *page;
    size_t count = 0U;

    /* A server older than 3.6.0 answers ERROR: the page is not built, so
     * a sync keeps the previous one */
    if (control_gather(path, "CLIENTS NAMES\n", &all) != 0) {
        return 1;
    }
    page = nginx_index_format(all, domain, stderr, &count);
    free(all);
    if (page == NULL) {
        fprintf(stderr, "camex: cannot build the start page (check "
                "--domain)\n");
        return 1;
    }
    fputs(page, stdout);
    fflush(stdout);
    free(page);
    return 0;
}

#else  /* _WIN32 */

int control_nginx_index(const char *path, const char *domain)
{
    (void)path;
    (void)domain;
    fprintf(stderr, "camex: --status is not available on Windows\n");
    return 1;
}

int control_nginx_map(const char *path, const char *domain, int port,
                      const char *dflt)
{
    (void)path;
    (void)domain;
    (void)port;
    (void)dflt;
    fprintf(stderr, "camex: --status is not available on Windows\n");
    return 1;
}

int control_status(const char *path, int json)
{
    (void)path;
    (void)json;
    fprintf(stderr, "camex: --status is not available on Windows\n");
    return 1;
}

#endif
