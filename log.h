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
    __attribute__((format(__mingw_printf__, 2, 3)));
#else
void log_message(int priority, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#endif

void print_errno_message(int priority, const char *what);

#endif /* CAMEX_LOG_H */
