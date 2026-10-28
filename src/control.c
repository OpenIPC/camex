/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * control.c — the server's local control socket (#82): camex --status
 *
 * The server never blocks on it: connections are non-blocking, a command
 * is one short line, the answer is built at once and sent as the socket
 * takes it, and a connection that takes longer than CONTROL_TIMEOUT
 * seconds is closed. WAIT (#130) is the exception: its answer waits until
 * the state of the clients changes or its own time is up.
 *
 */

#include "control.h"
#include "camex.h"
#include "log.h"
#include "server.h"
#include "config.h"
#include "nginxmap.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

/* WAIT holds a connection for a while: room for those and the others */
#define CONTROL_CONNS   8
#define CONTROL_TIMEOUT 10
#define CONTROL_CMD_MAX 64
#define CONTROL_END     ".\n"   /* last line of every answer */
#define CONTROL_WAIT_MAX 3600   /* WAIT: the longest wait, seconds */

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
    int waiting;            /* WAIT: no answer until a change or wait_until */
    uint64_t wait_hash;
    time_t wait_until;
} control_conn_t;

static int control_fd = -1;
static char control_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static char control_spec[sizeof(control_path)];   /* as asked: "", "none" */
static int control_spec_ok = 0;   /* control_spec is in effect */
static time_t control_paused_until = 0;   /* accept() failed: wait */
#define CONTROL_CONN_FREE { -1, "", 0U, NULL, 0U, 0U, 0, 0, 0U, 0 }
static control_conn_t control_conns[CONTROL_CONNS] = {
    CONTROL_CONN_FREE, CONTROL_CONN_FREE, CONTROL_CONN_FREE, CONTROL_CONN_FREE,
    CONTROL_CONN_FREE, CONTROL_CONN_FREE, CONTROL_CONN_FREE, CONTROL_CONN_FREE
};

static uint64_t control_state_hash(void);
static void control_wait_answer(control_conn_t *c, uint64_t h);

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
        if (c->waiting) {
            uint64_t h = control_state_hash();

            if (h != c->wait_hash || time(NULL) >= c->wait_until) {
                control_wait_answer(c, h);
                if (c->fd < 0) {
                    continue;
                }
                c->since = time(NULL);
            }
        } else if (difftime(time(NULL), c->since) > (double)CONTROL_TIMEOUT) {
            control_conn_close(c);
            continue;
        }
        /* A waiting one is read only to see the other side go away */
        FD_SET(c->fd, c->out != NULL ? writeset : readset);
        if (c->fd > maxfd) {
            maxfd = c->fd;
        }
    }
    return maxfd;
}

static void control_finish(control_conn_t *c, size_t len);

static void control_bad_command(control_conn_t *c, size_t *len)
{
    static const char err[] = "ERROR unknown command (STATUS, "
                              "STATUS JSON, CLIENTS, CLIENTS NAMES, "
                              "CLIENTS INDEX, DOMAIN, "
                              "WAIT <hash|-> <seconds>)\n";

    c->out = malloc(sizeof(err));
    if (c->out != NULL) {
        memcpy(c->out, err, sizeof(err));
        *len = sizeof(err) - 1U;
    }
}

/* The state's fingerprint (server_state_hash()), worked out again at
 * most every 250 ms while someone waits for it: a busy server runs its
 * loop for every packet */
static uint64_t state_hash;
static struct timespec state_hash_at;
static int state_hash_have = 0;

/* Worked out now: also what the next 250 ms answer with */
static uint64_t control_state_hash_now(void)
{
    state_hash = server_state_hash();
    clock_gettime(CLOCK_MONOTONIC, &state_hash_at);
    state_hash_have = 1;
    return state_hash;
}

static uint64_t control_state_hash(void)
{
    struct timespec now;
    long ms;

    clock_gettime(CLOCK_MONOTONIC, &now);
    ms = (long)(now.tv_sec - state_hash_at.tv_sec) * 1000L +
         (now.tv_nsec - state_hash_at.tv_nsec) / 1000000L;
    if (!state_hash_have || ms >= 250L || ms < 0L) {
        return control_state_hash_now();
    }
    return state_hash;
}

/* WAIT's answer: the state's fingerprint, 16 hex digits */
static void control_wait_answer(control_conn_t *c, uint64_t h)
{
    c->waiting = 0;
    c->out = malloc(18U);
    if (c->out != NULL) {
        snprintf(c->out, 18U, "%016" PRIx64 "\n", h);
    }
    control_finish(c, 17U);
}

/* WAIT <hash|-> <seconds> (#130): answers at once when the state's
 * fingerprint is not <hash> ("-": never), else when it changes or the
 * seconds are up; the answer is the fingerprint. -1: malformed */
static int control_wait_start(control_conn_t *c)
{
    const char *p = c->cmd + 5;
    char *end = NULL;
    uint64_t given = 0U;
    int any = 0;
    long secs;
    uint64_t h;

    if (*p == '-') {
        any = 1;
        p++;
    } else {
        unsigned long long v;

        errno = 0;
        v = strtoull(p, &end, 16);
        if (end == p || end - p > 16 || errno != 0) {
            return -1;
        }
        given = (uint64_t)v;
        p = end;
    }
    if (*p != ' ') {
        return -1;
    }
    secs = strtol(p + 1, &end, 10);
    if (end == p + 1 || *end != '\0' || secs < 0 || secs > CONTROL_WAIT_MAX) {
        return -1;
    }
    /* Not the one of the last 250 ms: the caller may have read the state
     * just now; and the cache gets this one, so that the loop's next look
     * does not compare with an older state */
    h = control_state_hash_now();
    if (any || h != given || secs == 0) {
        control_wait_answer(c, h);
        return 0;
    }
    c->waiting = 1;
    c->wait_hash = given;
    c->wait_until = time(NULL) + secs;
    return 0;
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
    if (strncmp(c->cmd, "WAIT ", 5U) == 0) {
        if (control_wait_start(c) == 0) {
            return;   /* waiting, or answered already */
        }
        c->out = NULL;   /* a malformed WAIT: the ERROR below */
        control_bad_command(c, &len);
    } else if (strcmp(c->cmd, "STATUS") == 0) {
        c->out = server_status(0, &len);
    } else if (strcmp(c->cmd, "STATUS JSON") == 0) {
        c->out = server_status(1, &len);
    } else if (strcmp(c->cmd, "CLIENTS") == 0) {
        c->out = server_clients_list(0, &len);
    } else if (strcmp(c->cmd, "CLIENTS NAMES") == 0) {
        c->out = server_clients_list(1, &len);
    } else if (strcmp(c->cmd, "CLIENTS INDEX") == 0) {
        c->out = server_clients_index(&len);
    } else if (strcmp(c->cmd, "DOMAIN") == 0) {
        /* domain= of [server] (#106), an empty line without one; in lower
         * case, as the map writes it (and never "ERROR...") */
        size_t k;

        len = strlen(server_db.globals.domain) + 1U;
        c->out = malloc(len + 1U);
        if (c->out != NULL) {
            for (k = 0U; k + 1U < len; ++k) {
                c->out[k] = (char)tolower(
                    (unsigned char)server_db.globals.domain[k]);
            }
            c->out[len - 1U] = '\n';
            c->out[len] = '\0';
        }
    } else {
        control_bad_command(c, &len);
    }
    control_finish(c, len);
}

/* An answer in c->out, len bytes: the end marker added, sent from now on */
static void control_finish(control_conn_t *c, size_t len)
{
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
        if (c->waiting && FD_ISSET(c->fd, readset)) {
            char drop[64];
            ssize_t n = recv(c->fd, drop, sizeof(drop), 0);

            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                           errno != EINTR)) {
                control_conn_close(c);
            }
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

/* Set while a question may go unanswered without a word: control_ask()
 * then says nothing about an ERROR answer (DOMAIN of a server older than
 * 3.8.0) or a socket nobody listens on; other failures it still names */
static int control_quiet = 0;

#define ASK_ERR(...) \
    do { \
        if (!control_quiet) { \
            fprintf(stderr, __VA_ARGS__); \
        } \
    } while (0)

/* Client side: send cmd to path; the whole answer, without its end
 * marker, in *buf (malloc'd, NUL-terminated). 0, or -1 with a message on
 * stderr (no answer, a cut-off one); -2 when nobody listens there; -3
 * when the server answered ERROR (an unknown command, say). */
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

        /* Quiet only for a socket nobody listens on (a server that is
         * gone): the clients' query names it */
        if (!control_quiet || (err != ECONNREFUSED && err != ENOENT)) {
            fprintf(stderr, "camex: cannot connect to %s: %s%s\n", path,
                    strerror(err), err == EACCES
                        ? " (run it as the server's user, normally root)"
                        : (err == ECONNREFUSED || err == ENOENT)
                              ? " (is the server running?)" : "");
        }
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
            ASK_ERR("camex: %s: %s", path, *buf);
            return -3;   /* the server answered: it does not know cmd */
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

/* text to stdout, whole: 0, or 1 with a message (a full disk, say), so a
 * script never takes a cut-off map or page for a good one */
static int control_put(const char *text, const char *what)
{
    if (fputs(text, stdout) == EOF || fflush(stdout) != 0 ||
        ferror(stdout)) {
        fprintf(stderr, "camex: cannot write the %s: %s\n", what,
                strerror(errno));
        return 1;
    }
    return 0;
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

/* The domain for the map and the page (#106): --domain (given), else
 * domain= of the servers' [server] block, in out. A warning on stderr when
 * they differ, when the servers differ, or when there is none. 0, or -1
 * (with a message) when a server did not answer */
static int control_domain(const char *path, const char *given, char *out,
                          size_t size)
{
    control_path_t one;
    control_path_t *paths = &one;
    control_path_t *listed = NULL;
    char server[254] = "";
    int n, i, differ = 0, none = 0, failed = 0;

    if (path != NULL && path[0] != '\0') {
        n = 1;
        snprintf(one, sizeof(one), "%s", path);
    } else {
        n = control_list(&listed);
        paths = listed;
        if (n <= 0) {
            /* No server: the clients' query says so, not a domain warning */
            free(listed);
            snprintf(out, size, "%s", given != NULL ? given : "");
            return 0;
        }
    }
    /* Quiet: a server older than 3.8.0 answers ERROR, one that is gone
     * does not answer; the clients' query says so */
    control_quiet = 1;
    for (i = 0; i < n; ++i) {
        char *buf = NULL;
        size_t len = 0U;

        int rc = control_ask(paths[i], "DOMAIN\n", &buf, &len);

        if (rc == -3) {
            none = 1;   /* a server older than 3.8.0: ERROR */
        } else if (rc == -1) {
            /* No answer (a timeout, a cut-off one): the map must not lose
             * this server's domain, so no map this time */
            failed = 1;
        } else if (rc == 0) {
            while (len > 0U && (buf[len - 1U] == '\n' ||
                                buf[len - 1U] == '\r')) {
                buf[--len] = '\0';
            }
            if (len > 0U && camex_domain_valid(buf)) {
                if (server[0] == '\0') {
                    snprintf(server, sizeof(server), "%s", buf);
                } else if (strcasecmp(server, buf) != 0) {
                    differ = 1;
                }
            } else {
                none = 1;   /* no domain= */
            }
        }
        free(buf);
    }
    control_quiet = 0;
    free(listed);
    if (failed) {
        fprintf(stderr, "camex: a server did not answer for its domain=: "
                "no map or page this time\n");
        return -1;
    }
    /* The servers disagree: all their clients get one domain */
    if (differ) {
        fprintf(stderr, "camex: the servers have different domain= (%s "
                "and another): %s used for all their clients\n", server,
                given != NULL && given[0] != '\0' ? given : server);
    } else if (none && server[0] != '\0') {
        fprintf(stderr, "camex: a server has no domain= while another has "
                "%s: %s used for all their clients\n", server,
                given != NULL && given[0] != '\0' ? given : server);
    }

    if (given != NULL && given[0] != '\0') {
        if (server[0] != '\0' && strcasecmp(server, given) != 0) {
            fprintf(stderr, "camex: --domain %s differs from the server's "
                    "domain= %s: %s used\n", given, server, given);
        }
        snprintf(out, size, "%s", given);
    } else {
        snprintf(out, size, "%s", server);
        if (server[0] == '\0') {
            fprintf(stderr, "camex: no domain (domain= in [server] or "
                    "--domain): the names are the bare client IDs\n");
        }
    }
    return 0;
}

int control_nginx_map(const char *path, const char *domain, int port,
                      const char *dflt)
{
    char *all = NULL, *map;
    size_t count = 0U;
    int rc;

    char dom[254];

    if (control_domain(path, domain, dom, sizeof(dom)) != 0 ||
        control_gather(path, "CLIENTS\n", &all) != 0) {
        return 1;
    }
    map = nginx_map_format(all, dom, port, dflt, stderr, &count);
    free(all);
    if (map == NULL) {
        fprintf(stderr, "camex: cannot build the map (check --domain, "
                "--upstream-port and --default)\n");
        return 1;
    }
    rc = control_put(map, "map");
    free(map);
    return rc;
}

int control_nginx_index(const char *path, const char *domain, int http_port)
{
    char *all = NULL, *page;
    size_t count = 0U;
    int rc;

    /* A server older than 3.7.0 answers ERROR: the page is not built, so
     * a sync keeps the previous one */
    char dom[254];

    if (control_domain(path, domain, dom, sizeof(dom)) != 0 ||
        control_gather(path, "CLIENTS INDEX\n", &all) != 0) {
        return 1;
    }
    page = nginx_index_format(all, dom, http_port, stderr, &count);
    free(all);
    if (page == NULL) {
        fprintf(stderr, "camex: cannot build the start page (check "
                "--domain)\n");
        return 1;
    }
    rc = control_put(page, "start page");
    free(page);
    return rc;
}

/* One server of camex --status --wait */
typedef struct {
    control_path_t path;
    uint64_t old;           /* its fingerprint in the token */
    int has_old;
    uint64_t now;           /* its answer */
    int has_now;
    int fd;                 /* -1: answered, or gone */
    int gone;
    char buf[64];
    size_t len;
    dev_t dev;              /* the socket file at the start: another one */
    ino_t ino;              /* at its path is a server started there; */
    time_t ctime;           /* (the inode may be the old one's, reused) */
    int have_ino;
} control_waiter_t;

/* A socket path as the token has it: ',' and '%' as %2C and %25, so that
 * any path makes one entry. 0, or -1 when it does not fit */
static int token_path(const char *path, char *out, size_t size)
{
    size_t o = 0U;

    for (; *path != '\0'; ++path) {
        const char *e = *path == ',' ? "%2C" : *path == '%' ? "%25" : NULL;
        size_t n = e != NULL ? 3U : 1U;

        if (o + n >= size) {
            return -1;
        }
        if (e != NULL) {
            memcpy(out + o, e, 3U);
        } else {
            out[o] = *path;
        }
        o += n;
    }
    out[o] = '\0';
    return 0;
}

/* The fingerprint of path in the token ("<hash>:<path>,..."): 1 and *h,
 * or 0 */
static int token_find(const char *token, const char *rawpath, uint64_t *h)
{
    const char *p = token;
    char path[3U * sizeof(control_path_t)];
    size_t plen;

    if (token_path(rawpath, path, sizeof(path)) != 0) {
        return 0;
    }
    plen = strlen(path);

    while (p != NULL && *p != '\0') {
        const char *colon = strchr(p, ':');
        const char *comma = strchr(p, ',');
        size_t elen = comma != NULL ? (size_t)(comma - p) : strlen(p);

        if (colon != NULL && colon < p + elen && colon - p == 16 &&
            (size_t)(p + elen - colon - 1) == plen &&
            memcmp(colon + 1, path, plen) == 0) {
            char hex[17];
            char *end = NULL;

            memcpy(hex, p, 16U);
            hex[16] = '\0';
            *h = (uint64_t)strtoull(hex, &end, 16);
            return end == hex + 16;
        }
        p = comma != NULL ? comma + 1 : NULL;
    }
    return 0;
}

/* Is a server of the token gone from the list? */
static int token_lost(const char *token, const control_waiter_t *w, int n)
{
    const char *p = token;

    while (p != NULL && *p != '\0') {
        const char *colon = strchr(p, ':');
        const char *comma = strchr(p, ',');
        size_t elen = comma != NULL ? (size_t)(comma - p) : strlen(p);
        int i, found = 0;

        if (colon == NULL || colon >= p + elen) {
            return 1;   /* not a token of ours: start again */
        }
        for (i = 0; i < n; ++i) {
            char path[3U * sizeof(control_path_t)];
            size_t plen;

            if (token_path(w[i].path, path, sizeof(path)) != 0) {
                continue;
            }
            plen = strlen(path);
            if ((size_t)(p + elen - colon - 1) == plen &&
                memcmp(colon + 1, path, plen) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            return 1;
        }
        p = comma != NULL ? comma + 1 : NULL;
    }
    return 0;
}

/* A whole WAIT answer in w->buf: 1 with w->now, 0 not yet, -1 malformed,
 * -3 ERROR (a server before #130) */
static int waiter_parse(control_waiter_t *w)
{
    char *end = NULL;

    w->buf[w->len] = '\0';
    if (strncmp(w->buf, "ERROR", 5U) == 0) {
        return -3;
    }
    if (w->len < 19U) {
        return 0;
    }
    if (w->buf[16] != '\n' || strcmp(w->buf + 17, CONTROL_END) != 0) {
        return -1;
    }
    w->now = (uint64_t)strtoull(w->buf, &end, 16);
    if (end != w->buf + 16) {
        return -1;
    }
    w->has_now = 1;
    return 1;
}

/* A control socket that is not one of w's, or another file at the path
 * of one (a server started where one left its socket behind): a server
 * started since the list was read (#138) */
static int wait_new_socket(const control_waiter_t *w, int n)
{
    control_path_t *paths = NULL;
    int m = control_list(&paths), i, j, found = 0;

    for (i = 0; i < m && !found; ++i) {
        int known = 0;

        for (j = 0; j < n; ++j) {
            if (strcmp(paths[i], w[j].path) == 0) {
                struct stat st;

                known = !w[j].have_ino || stat(paths[i], &st) != 0 ||
                        (st.st_dev == w[j].dev && st.st_ino == w[j].ino &&
                         st.st_ctime == w[j].ctime);
                break;
            }
        }
        found = !known;
    }
    free(paths);
    return found;
}

/* Nothing to wait on (no server, or only sockets left behind): wait the
 * seconds, looking every second for a server that starts (not with
 * --control: one server only). 1 when one did */
static int wait_for_server(const control_waiter_t *w, int n, int secs,
                           int scoped)
{
    time_t deadline = time(NULL) + secs;

    while (time(NULL) < deadline) {
        sleep(1);
        if (!scoped && wait_new_socket(w, n)) {
            return 1;
        }
    }
    return 0;
}

/* The WAIT command to fd, non-blocking, by the deadline: 0 sent, 1 the
 * server is busy (it counts as unchanged), -1 it is gone */
static int wait_send(int fd, const char *cmd, time_t deadline)
{
    size_t len = strlen(cmd), off = 0U;

    while (off < len) {
        ssize_t k;
#ifdef MSG_NOSIGNAL
        k = send(fd, cmd + off, len - off, MSG_NOSIGNAL);
#else
        k = send(fd, cmd + off, len - off, 0);
#endif
        if (k > 0) {
            off += (size_t)k;
        } else if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                             errno == EINTR)) {
            fd_set wset;
            struct timeval tv;
            time_t left = deadline - time(NULL);

            if (left <= 0) {
                return 1;
            }
            FD_ZERO(&wset);
            FD_SET(fd, &wset);
            tv.tv_sec = left;
            tv.tv_usec = 0;
            if (select(fd + 1, NULL, &wset, NULL, &tv) <= 0 && errno != EINTR) {
                return 1;
            }
        } else {
            return -1;
        }
    }
    return 0;
}

int control_wait(const char *path, const char *token, int timeout)
{
    control_path_t *paths = NULL;
    control_waiter_t *w;
    int n, i, changed = 0, rc = 0;
    int secs;
    time_t deadline;
    int printed = 0;
    int snapshot = 0;   /* "-": the state now, no waiting at all */
    int scoped = path != NULL && path[0] != '\0';

    if (token == NULL || strcmp(token, "-") == 0) {
        token = "";
        snapshot = 1;
    } else if (strcmp(token, "none") == 0) {
        token = "";   /* no server last time: wait for one */
    }
    if (path != NULL && path[0] != '\0') {
        if (strlen(path) >= sizeof(control_path_t)) {
            fprintf(stderr, "camex: control socket path too long: %s\n",
                    path);
            return 1;
        }
        paths = malloc(sizeof(*paths));
        if (paths == NULL) {
            return 1;
        }
        snprintf(paths[0], sizeof(paths[0]), "%s", path);
        n = 1;
    } else {
        n = control_list(&paths);
        if (n < 0) {
            n = 0;   /* no directory: no server yet */
        }
    }
    w = calloc(n > 0 ? (size_t)n : 1U, sizeof(*w));
    if (w == NULL) {
        free(paths);
        return 1;
    }
    for (i = 0; i < n; ++i) {
        memcpy(w[i].path, paths[i], sizeof(w[i].path));
        w[i].fd = -1;
        w[i].has_old = token_find(token, w[i].path, &w[i].old);
        {
            struct stat st;

            if (stat(w[i].path, &st) == 0) {
                w[i].dev = st.st_dev;
                w[i].ino = st.st_ino;
                w[i].ctime = st.st_ctime;
                w[i].have_ino = 1;
            }
        }
    }
    free(paths);
    /* A server gone since the token, or a token not of ours: a change.
     * With --control only that server counts: a token of all of them may
     * name others */
    changed = scoped ? 0 : token_lost(token, w, n);
    if (n == 0) {
        if (!changed && !snapshot) {
            /* no server: wait for one; one that starts answers with its
             * state at once */
            if (wait_for_server(w, n, timeout, scoped)) {
                free(w);
                return control_wait(path, "-", timeout);
            }
        }
        printf("none\n");
        free(w);
        return 0;
    }
    /* Changed already: only the fingerprints of now, no waiting */
    secs = changed ? 0 : timeout;
    deadline = time(NULL) + secs + 5;
    for (i = 0; i < n; ++i) {
        struct sockaddr_un sa;
        char cmd[48];
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);

        if (fd < 0) {
            fprintf(stderr, "camex: socket: %s\n", strerror(errno));
            rc = 1;
            break;
        }
#ifdef SO_NOSIGPIPE
        {
            int one = 1;

            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        }
#endif
        memset(&sa, 0, sizeof(sa));
        sa.sun_family = AF_UNIX;
        snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", w[i].path);
        /* Never blocks: a server whose backlog is full must not hold up
         * the others (it counts as unchanged, its fingerprint kept) */
        if (fd >= FD_SETSIZE || set_nonblock(fd) != 0 ||
            connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
            int err = fd >= FD_SETSIZE ? EMFILE : errno;

            close(fd);
            if (err == EAGAIN || err == EWOULDBLOCK || err == EINPROGRESS) {
                continue;   /* busy: as it was */
            }
            if (err == ECONNREFUSED || err == ENOENT) {
                /* A socket left behind: a change only for a server the
                 * token had (else every round would be one) */
                w[i].gone = 1;
                changed |= w[i].has_old;
                continue;
            }
            fprintf(stderr, "camex: cannot connect to %s: %s%s\n", w[i].path,
                    strerror(err), err == EACCES
                        ? " (run it as the server's user, normally root)" : "");
            rc = 1;
            break;
        }
        if (w[i].has_old) {
            snprintf(cmd, sizeof(cmd), "WAIT %016" PRIx64 " %d\n", w[i].old,
                     secs);
        } else {
            snprintf(cmd, sizeof(cmd), "WAIT - 0\n");
        }
        {
            /* A second at most: 30 bytes on a new connection */
            int sent = wait_send(fd, cmd, time(NULL) + 1);

            if (sent != 0) {
                close(fd);
                if (sent < 0) {
                    w[i].gone = 1;
                    changed |= w[i].has_old;
                }
                continue;   /* busy: as it was */
            }
        }
        w[i].fd = fd;
    }
    /* Only sockets left behind (or a --control nobody listens on), and no
     * change: wait as with no server at all, or a follower would spin */
    if (rc == 0 && !changed && !snapshot) {
        int open_fds = 0;

        for (i = 0; i < n; ++i) {
            open_fds += w[i].fd >= 0;
        }
        /* What is left of the wait, not all of it again */
        if (open_fds == 0 && deadline - 5 > time(NULL) &&
            wait_for_server(w, n, (int)(deadline - 5 - time(NULL)), scoped)) {
            changed = 1;
        }
    }
    while (rc == 0) {
        fd_set rset;
        struct timeval tv;
        int maxfd = -1, waiting_new = 0, open_fds = 0, ready;
        time_t left;

        FD_ZERO(&rset);
        for (i = 0; i < n; ++i) {
            if (w[i].fd >= 0) {
                FD_SET(w[i].fd, &rset);
                if (w[i].fd > maxfd) {
                    maxfd = w[i].fd;
                }
                open_fds++;
                if (!w[i].has_old) {
                    waiting_new++;
                }
            }
        }
        /* Done: every server answered, or one changed and the new ones
         * (they answer at once) have told their fingerprints */
        if (open_fds == 0 || (changed && waiting_new == 0)) {
            break;
        }
        left = deadline - time(NULL);
        if (left <= 0) {
            break;
        }
        /* A second at a time: a server that starts meanwhile is a change
         * (#138) */
        tv.tv_sec = left < 1 ? left : 1;
        tv.tv_usec = 0;
        ready = select(maxfd + 1, &rset, NULL, NULL, &tv);
        if (ready == 0 && !scoped && wait_new_socket(w, n)) {
            changed = 1;
            break;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            rc = 1;
            break;
        }
        for (i = 0; i < n && ready > 0; ++i) {
            ssize_t got;
            int r;

            if (w[i].fd < 0 || !FD_ISSET(w[i].fd, &rset)) {
                continue;
            }
            got = recv(w[i].fd, w[i].buf + w[i].len,
                       sizeof(w[i].buf) - 1U - w[i].len, 0);
            if (got < 0 && (errno == EINTR || errno == EAGAIN)) {
                continue;
            }
            if (got > 0) {
                w[i].len += (size_t)got;
            }
            r = waiter_parse(&w[i]);
            if (r == -3) {
                fprintf(stderr, "camex: %s: the server does not know WAIT "
                        "(camex 3.11.0 or later)\n", w[i].path);
                rc = 3;
                break;
            }
            if (r == 0 && got > 0 && w[i].len < sizeof(w[i].buf) - 1U) {
                continue;   /* more to come */
            }
            close(w[i].fd);
            w[i].fd = -1;
            if (r != 1) {
                w[i].gone = 1;   /* stopped while we waited */
                changed = 1;
            } else if (!w[i].has_old || w[i].now != w[i].old) {
                changed = 1;
            }
        }
    }
    for (i = 0; i < n; ++i) {
        if (w[i].fd >= 0) {
            close(w[i].fd);
        }
    }
    if (rc == 0) {
        /* The new token: the answers, else what the token had */
        for (i = 0; i < n; ++i) {
            uint64_t h;

            if (w[i].gone || (!w[i].has_now && !w[i].has_old)) {
                continue;
            }
            char enc[3U * sizeof(control_path_t)];

            if (token_path(w[i].path, enc, sizeof(enc)) != 0) {
                continue;
            }
            h = w[i].has_now ? w[i].now : w[i].old;
            printf("%s%016" PRIx64 ":%s", printed ? "," : "", h, enc);
            printed = 1;
        }
        printf("%s\n", printed ? "" : "none");
    }
    free(w);
    return rc;
}

#else  /* _WIN32 */

int control_wait(const char *path, const char *token, int timeout)
{
    (void)path;
    (void)token;
    (void)timeout;
    fprintf(stderr, "camex: --status is not available on Windows\n");
    return 1;
}

int control_nginx_index(const char *path, const char *domain, int http_port)
{
    (void)path;
    (void)domain;
    (void)http_port;
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
