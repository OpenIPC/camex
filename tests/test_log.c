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
    log_limit_t limit = { 0 };   /* every member its zero value */
    int failures = 0;
    unsigned int i, tries;
    long before, after;

    /* A burst within one second: LOG_LIMIT_BURST logged, the rest counted.
     * Retry when the burst crossed a second boundary (#90): the second
     * before the first message and after the last one must be the same.
     * Checking the window after the burst is not enough: a burst that
     * starts at the end of a second ends in a new window that looks fine
     * but holds one message less. Each try starts right after a second
     * begins, so the burst has the whole second; a host so slow that every
     * burst takes longer skips the check rather than failing it. */
    for (tries = 0U; tries < 10U; ++tries) {
        struct timespec pause = { 0, 1000000L };   /* 1 ms */

        before = (long)time(NULL);
        while ((long)time(NULL) == before) {
            nanosleep(&pause, NULL);
        }
        limit.window = 0;
        limit.count = 0U;
        limit.suppressed = 0U;
        before = (long)time(NULL);
        for (i = 0U; i < 20U; ++i) {
            log_limited(&limit, LOG_DEBUG, "dropped packet %u", i);
        }
        after = (long)time(NULL);
        if (before == after && limit.window == before) {
            break;
        }
    }
    if (tries == 10U) {
        printf("test_log: SKIP burst: every burst took longer than a "
               "second on this host\n");
    } else if (limit.count != LOG_LIMIT_BURST ||
               limit.suppressed != 20U - LOG_LIMIT_BURST ||
               limit.suppressed_priority != LOG_DEBUG) {
        fprintf(stderr, "FAIL: burst: count=%u suppressed=%u\n",
                limit.count, limit.suppressed);
        ++failures;
    }

    /* The summary keeps the most severe priority among the suppressed
     * (made deterministic: the window is the current second) */
    limit.window = (long)time(NULL);
    limit.count = LOG_LIMIT_BURST;
    limit.suppressed = 1U;
    limit.suppressed_priority = LOG_DEBUG;
    log_limited(&limit, LOG_ERR, "an error");
    if (limit.window == (long)time(NULL) &&
        limit.suppressed_priority != LOG_ERR) {
        fprintf(stderr, "FAIL: suppressed priority %d, want %d\n",
                limit.suppressed_priority, LOG_ERR);
        ++failures;
    }

    /* log_limit_tick() reports a window that has ended */
    limit.window -= 1;
    log_limit_tick();
    if (limit.suppressed != 0U) {
        fprintf(stderr, "FAIL: tick left %u suppressed\n", limit.suppressed);
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
