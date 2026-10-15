/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * log.h — logging helpers (syslog + stderr/stdout)
 *
 */

#ifndef CAMEX_LOG_H
#define CAMEX_LOG_H

#ifndef _WIN32
#include <sys/syslog.h>
#else
#define LOG_EMERG   0
#define LOG_ALERT   1
#define LOG_CRIT    2
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6
#define LOG_DEBUG   7
#define LOG_PID     0x01
#define LOG_DAEMON  (3<<3)
#endif

#include <stdio.h>

#ifdef __MINGW32__
void log_message(int priority, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
void log_message(int priority, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#endif

void print_errno_message(int priority, const char *what);

/* Rate limit for messages that a peer can trigger at will, e.g. one per
 * dropped packet (#16 L1): at most LOG_LIMIT_BURST messages per second per
 * call site. The number suppressed is logged, at the highest priority among
 * them, when the next second starts or from log_limit_tick(). */
#define LOG_LIMIT_BURST 5U

typedef struct {
    long window;            /* second of the current window */
    unsigned int count;     /* messages logged in this window */
    unsigned int suppressed;
    int suppressed_priority;
    const char *fmt;        /* names the message in the summary */
    unsigned char listed;   /* known to log_limit_tick() */
} log_limit_t;

void log_limited(log_limit_t *limit, int priority, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Report suppressed counts of windows that have ended; call it from the
 * event loop so the count of a flood that stopped is not lost. */
void log_limit_tick(void);

/* log_peer(priority, fmt, ...): log_limited() with a limiter of its own
 * for each call site, so one kind of message cannot hide another. */
#define log_peer(...) do { \
        static log_limit_t log_peer_limit_; \
        log_limited(&log_peer_limit_, __VA_ARGS__); \
    } while (0)

#endif /* CAMEX_LOG_H */
