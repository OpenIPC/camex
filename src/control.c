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

void control_default_path(char *buf, size_t size, int transport, int port)
{
    snprintf(buf, size, "%s/camex-%s-%d.sock", CAMEX_CONTROL_DIR,
             transport == CAMEX_TRANSPORT_TCP ? "tcp" : "udp", port);
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
 * cleanly) */
static int stale_socket(const char *path)
{
    struct sockaddr_un sa;
    struct stat st;
    int fd, rc;

    if (lstat(path, &st) != 0 || !S_ISSOCK(st.st_mode)) {
        return 0;
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return 0;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    close(fd);
    return rc != 0 && errno == ECONNREFUSED;
}

int control_open(const char *path, int transport, int port)
{
    struct sockaddr_un sa;
    char dir[sizeof(control_path)];
    char *slash;
    mode_t old_mask;
    int fd;

    if (path != NULL && strcmp(path, "none") == 0) {
        return 0;
    }
    if (path == NULL || path[0] == '\0') {
        control_default_path(control_path, sizeof(control_path), transport,
                             port);
    } else if (strlen(path) >= sizeof(control_path)) {
        log_message(LOG_WARNING, "Control socket path too long: %s "
                    "(camex --status will not reach this server)", path);
        return -1;
    } else {
        snprintf(control_path, sizeof(control_path), "%s", path);
    }

    /* Its directory: created when missing (CAMEX_CONTROL_DIR is on a
     * tmpfs, empty after a reboot) */
    snprintf(dir, sizeof(dir), "%s", control_path);
    slash = strrchr(dir, '/');
    if (slash != NULL && slash != dir) {
        *slash = '\0';
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
            log_message(LOG_WARNING, "Cannot create %s: %s (camex --status "
                        "will not reach this server; control_socket= sets "
                        "another path)", dir, strerror(errno));
            control_path[0] = '\0';
            return -1;
        }
    }
    if (stale_socket(control_path)) {
        unlink(control_path);
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        log_message(LOG_WARNING, "Control socket: %s", strerror(errno));
        control_path[0] = '\0';
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", control_path);
    /* 0600 from the start: no moment in which others could connect */
    old_mask = umask(0177);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        int err = errno;

        umask(old_mask);
        close(fd);
        log_message(LOG_WARNING, "Cannot listen on %s: %s%s", control_path,
                    strerror(err), err == EADDRINUSE
                        ? " (another server uses it: set control_socket=)"
                        : "");
        control_path[0] = '\0';
        return -1;
    }
    umask(old_mask);
    if (listen(fd, CONTROL_CONNS) != 0 || set_nonblock(fd) != 0) {
        log_message(LOG_WARNING, "Control socket %s: %s", control_path,
                    strerror(errno));
        close(fd);
        unlink(control_path);
        control_path[0] = '\0';
        return -1;
    }
    control_fd = fd;
    log_message(LOG_INFO, "Control socket: %s (camex --status)",
                control_path);
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

int control_fdset(fd_set *readset, fd_set *writeset, int maxfd)
{
    size_t i;

    if (control_fd < 0) {
        return maxfd;
    }
    FD_SET(control_fd, readset);
    if (control_fd > maxfd) {
        maxfd = control_fd;
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
    } else {
        static const char err[] = "ERROR unknown command (STATUS, "
                                  "STATUS JSON)\n";

        c->out = malloc(sizeof(err));
        if (c->out != NULL) {
            memcpy(c->out, err, sizeof(err));
            len = sizeof(err) - 1U;
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
                break;
            }
            for (i = 0U; i < CONTROL_CONNS; ++i) {
                if (control_conns[i].fd < 0) {
                    c = &control_conns[i];
                    break;
                }
            }
            /* select() watches only descriptors below FD_SETSIZE */
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

/* Client side: send cmd to path, copy the answer to out (NULL: stdout).
 * 0, or -1 with a message on stderr. */
static int control_ask(const char *path, const char *cmd, FILE *out,
                       char **buf, size_t *buf_len)
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
        return -1;
    }
    if (send(fd, cmd, strlen(cmd), 0) != (ssize_t)strlen(cmd)) {
        fprintf(stderr, "camex: %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    while ((n = recv(fd, chunk, sizeof(chunk), 0)) > 0) {
        if (out != NULL) {
            fwrite(chunk, 1U, (size_t)n, out);
        } else {
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
    }
    close(fd);
    return n < 0 ? -1 : 0;
}

/* The socket paths in CAMEX_CONTROL_DIR, sorted; count, or -1 */
static int control_list(char paths[][108], int max)
{
    DIR *d = opendir(CAMEX_CONTROL_DIR);
    struct dirent *e;
    int n = 0, i, j;

    if (d == NULL) {
        return -1;
    }
    while ((e = readdir(d)) != NULL && n < max) {
        size_t len = strlen(e->d_name);
        struct stat st;

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
    char paths[32][108];
    int n, i, ok = 0;

    if (path != NULL && path[0] != '\0') {
        n = 1;
        snprintf(paths[0], sizeof(paths[0]), "%s", path);
    } else {
        n = control_list(paths, 32);
        if (n <= 0) {
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

            if (control_ask(paths[i], cmd, NULL, &buf, &len) == 0 &&
                buf != NULL) {
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
            if (control_ask(paths[i], cmd, stdout, NULL, NULL) == 0) {
                ok++;
            }
        }
    }
    if (json) {
        fputs("]}\n", stdout);
    }
    fflush(stdout);
    return ok == n ? 0 : 1;
}

#else  /* _WIN32 */

int control_status(const char *path, int json)
{
    (void)path;
    (void)json;
    fprintf(stderr, "camex: --status is not available on Windows\n");
    return 1;
}

#endif
