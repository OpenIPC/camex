/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * test_log.c — unit tests for the log rate limiter
 *
 */

#include "log.h"

#include <stdio.h>
#include <time.h>

int main(void)
{
    log_limit_t limit = { 0, 0U, 0U };
    int failures = 0;
    unsigned int i;
    long start = (long)time(NULL);

    /* A burst within one second: LOG_LIMIT_BURST logged, the rest counted.
     * Retry if the burst happened to cross a second boundary. */
    for (;;) {
        limit.window = 0;
        limit.count = 0U;
        limit.suppressed = 0U;
        for (i = 0U; i < 20U; ++i) {
            log_limited(&limit, LOG_DEBUG, "dropped packet %u", i);
        }
        if (limit.window == (long)time(NULL) || (long)time(NULL) - start > 3) {
            break;
        }
    }
    if (limit.count != LOG_LIMIT_BURST ||
        limit.suppressed != 20U - LOG_LIMIT_BURST) {
        fprintf(stderr, "FAIL: burst: count=%u suppressed=%u\n",
                limit.count, limit.suppressed);
        ++failures;
    }

    /* A new second starts a new window and reports the suppressed ones */
    limit.window -= 1;
    log_limited(&limit, LOG_DEBUG, "next second");
    if (limit.count != 1U || limit.suppressed != 0U) {
        fprintf(stderr, "FAIL: new window: count=%u suppressed=%u\n",
                limit.count, limit.suppressed);
        ++failures;
    }

    if (failures != 0) {
        fprintf(stderr, "test_log: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_log: OK\n");
    return 0;
}
