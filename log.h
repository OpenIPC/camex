/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * log.h — logging helpers (syslog + stderr/stdout)
 *
 */

#ifndef CAMEX_LOG_H
#define CAMEX_LOG_H

#include <sys/syslog.h>

void log_message(int priority, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

void print_errno_message(int priority, const char *what);

#endif /* CAMEX_LOG_H */
