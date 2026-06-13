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
#include <sys/syslog.h>

void log_message(int priority, const char *fmt, ...)
{
    char message[512];
    va_list ap;
    FILE *stream = (priority <= LOG_WARNING) ? stderr : stdout;

    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    syslog(priority, "%s", message);
    fprintf(stream, "%s\n", message);
}

void print_errno_message(int priority, const char *what)
{
    log_message(priority, "%s: %s", what, strerror(errno));
}
