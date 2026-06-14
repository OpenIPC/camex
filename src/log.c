/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * log.c — logging helpers
 *
 */

#include "log.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <sys/syslog.h>
#endif

void log_message(int priority, const char *fmt, ...)
{
    char message[512];
    va_list ap;
    FILE *stream = (priority <= LOG_WARNING) ? stderr : stdout;

    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

#ifndef _WIN32
    syslog(priority, "%s", message);
#endif
    fprintf(stream, "%s\n", message);
}

void print_errno_message(int priority, const char *what)
{
    log_message(priority, "%s: %s", what, strerror(errno));
}
