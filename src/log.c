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
#include <time.h>
#ifndef _WIN32
#include <sys/syslog.h>
#endif
#ifdef __ANDROID__
#include <android/log.h>
#endif

void log_message(int priority, const char *fmt, ...)
{
    char message[512];
    va_list ap;
    FILE *stream = (priority <= LOG_WARNING) ? stderr : stdout;

    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

#ifdef __ANDROID__
    /* In the app stdout/stderr and syslog go nowhere: use logcat */
    __android_log_write(priority <= LOG_ERR ? ANDROID_LOG_ERROR :
                        priority <= LOG_WARNING ? ANDROID_LOG_WARN :
                        priority <= LOG_INFO ? ANDROID_LOG_INFO :
                        ANDROID_LOG_DEBUG, "camex", message);
#elif !defined(_WIN32)
    syslog(priority, "%s", message);
#endif
    fprintf(stream, "%s\n", message);
    fflush(stream);
}

#define LOG_LIMITS_MAX 64U

static log_limit_t *log_limits[LOG_LIMITS_MAX];
static unsigned int log_limits_count = 0U;

static void log_limit_report(log_limit_t *limit)
{
    if (limit->suppressed > 0U) {
        log_message(limit->suppressed_priority,
                    "(%u more \"%.60s\" messages suppressed)",
                    limit->suppressed, limit->fmt);
    }
    limit->suppressed = 0U;
}

void log_limited(log_limit_t *limit, int priority, const char *fmt, ...)
{
    char message[512];
    va_list ap;
    long now = (long)time(NULL);

    if (!limit->listed && log_limits_count < LOG_LIMITS_MAX) {
        log_limits[log_limits_count++] = limit;
        limit->listed = 1U;
    }
    limit->fmt = fmt;
    if (limit->window != now) {
        log_limit_report(limit);
        limit->window = now;
        limit->count = 0U;
    }
    if (limit->count >= LOG_LIMIT_BURST) {
        if (limit->suppressed == 0U ||
            priority < limit->suppressed_priority) {
            limit->suppressed_priority = priority;  /* lower = more severe */
        }
        limit->suppressed++;
        return;
    }
    limit->count++;

    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    log_message(priority, "%s", message);
}

void log_limit_tick(void)
{
    long now = (long)time(NULL);
    unsigned int i;

    for (i = 0U; i < log_limits_count; ++i) {
        if (log_limits[i]->suppressed > 0U && log_limits[i]->window != now) {
            log_limit_report(log_limits[i]);
        }
    }
}

void print_errno_message(int priority, const char *what)
{
    log_message(priority, "%s: %s", what, strerror(errno));
}
