/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * test_cpaceguard.c — limits on CPace attempts (cpaceguard.h): a real
 *                     client that confirms spends nothing, a guesser runs
 *                     into the per-address limit, the total limit and the
 *                     ban; an address with a confirmed client is never
 *                     banned; 0 turns a limit off
 *
 */

#include "cpaceguard.h"

#include <stdio.h>

static int failures = 0;

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL: " __VA_ARGS__); \
            fputc('\n', stderr); \
            ++failures; \
        } \
    } while (0)

#define IP(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                        ((uint32_t)(c) << 8) | (uint32_t)(d))

int main(void)
{
    time_t t = 1000000;
    cpace_guard_stats_t s;
    int banned = 0;
    int i;
    int ok;

    /* A real client: attempt and confirm, many times, never refused */
    cpace_guard_config(5U, 10U, 8U, 60U);
    cpace_guard_reset();
    for (i = 0; i < 100; ++i) {
        ok = cpace_guard_attempt(IP(10, 0, 0, 1), t, &banned, 0);
        CHECK(ok == CPACE_GUARD_OK, "a confirming client refused at %d (%d)", i, ok);
        cpace_guard_confirm(IP(10, 0, 0, 1), t);
    }
    cpace_guard_stats(&s, t);
    CHECK(s.attempts == 100UL && s.confirmed == 100UL && s.refused == 0UL,
          "counts of a confirming client: %lu %lu %lu", s.attempts,
          s.confirmed, s.refused);

    /* A guesser: 5 a minute from one address, then refused */
    cpace_guard_reset();
    for (i = 0; i < 5; ++i) {
        CHECK(cpace_guard_attempt(IP(10, 0, 0, 2), t, NULL, 0) == CPACE_GUARD_OK,
              "guess %d refused early", i);
    }
    CHECK(cpace_guard_attempt(IP(10, 0, 0, 2), t, NULL, 0) == CPACE_GUARD_IP_LIMIT,
          "the 6th guess in a minute taken");
    /* 12 s later one more attempt (5 a minute) */
    CHECK(cpace_guard_attempt(IP(10, 0, 0, 2), t + 12, NULL, 0) == CPACE_GUARD_OK,
          "the bucket does not fill again");

    /* The ban: 8 attempts over TCP in a row (refused ones count) */
    cpace_guard_reset();
    for (i = 0; i < 7; ++i) {
        (void)cpace_guard_attempt(IP(10, 0, 0, 3), t + i, &banned, 1);
        CHECK(!banned, "banned after %d attempts", i + 1);
    }
    CHECK(cpace_guard_attempt(IP(10, 0, 0, 3), t + 7, &banned, 1) ==
              CPACE_GUARD_BANNED && banned,
          "not banned after 8 attempts in a row");
    CHECK(cpace_guard_attempt(IP(10, 0, 0, 3), t + 30, &banned, 1) ==
              CPACE_GUARD_BANNED && !banned,
          "the ban does not hold, or is made again");
    cpace_guard_stats(&s, t + 30);
    CHECK(s.bans == 1UL && s.banned_now == 1U, "ban counts: %lu %u", s.bans,
          s.banned_now);
    CHECK(cpace_guard_attempt(IP(10, 0, 0, 3), t + 7 + 61, &banned, 1) ==
              CPACE_GUARD_OK,
          "the ban does not end after its time");
    cpace_guard_stats(&s, t + 7 + 61);
    CHECK(s.banned_now == 0U, "an ended ban still counted");

    /* A NAT with a real camera behind it: limited, never banned */
    cpace_guard_reset();
    (void)cpace_guard_attempt(IP(10, 0, 0, 4), t, NULL, 0);
    cpace_guard_confirm(IP(10, 0, 0, 4), t);
    for (i = 0; i < 40; ++i) {
        (void)cpace_guard_attempt(IP(10, 0, 0, 4), t + 1 + i, &banned, 1);
        CHECK(!banned, "an address with a confirmed client banned");
    }
    CHECK(cpace_guard_attempt(IP(10, 0, 0, 4), t + 100, NULL, 0) !=
              CPACE_GUARD_BANNED,
          "an address with a confirmed client refused as banned");

    /* The total: 10 a minute from all addresses together */
    cpace_guard_reset();
    for (i = 0; i < 10; ++i) {
        CHECK(cpace_guard_attempt(IP(10, 1, 0, (unsigned)i), t, NULL, 0) ==
                  CPACE_GUARD_OK,
              "attempt %d of many addresses refused early", i);
    }
    CHECK(cpace_guard_attempt(IP(10, 1, 0, 200), t, NULL, 0) ==
              CPACE_GUARD_TOTAL_LIMIT,
          "the 11th attempt of all addresses taken");
    /* ... but not for an address whose client confirmed within the hour */
    (void)cpace_guard_attempt(IP(10, 0, 0, 9), t - 10, NULL, 0);
    cpace_guard_confirm(IP(10, 0, 0, 9), t - 10);
    CHECK(cpace_guard_attempt(IP(10, 0, 0, 9), t, NULL, 0) == CPACE_GUARD_OK,
          "a known-good address held by the total limit");

    /* 0 turns them off */
    cpace_guard_config(0U, 0U, 0U, 0U);
    cpace_guard_reset();
    for (i = 0; i < 1000; ++i) {
        ok = cpace_guard_attempt(IP(10, 0, 0, 5), t, &banned, 0);
        CHECK(ok == CPACE_GUARD_OK && !banned, "limits off, refused at %d", i);
        if (ok != CPACE_GUARD_OK) {
            break;
        }
    }

    /* Many addresses: the table never refuses a new one for lack of room */
    cpace_guard_config(5U, 0U, 8U, 60U);
    cpace_guard_reset();
    for (i = 0; i < 5000; ++i) {
        ok = cpace_guard_attempt(IP(172, 16, (unsigned)(i >> 8), (unsigned)(i & 0xFF)),
                                 t, NULL, 0);
        CHECK(ok == CPACE_GUARD_OK, "address %d refused (table full?)", i);
        if (ok != CPACE_GUARD_OK) {
            break;
        }
    }

    /* #207: UDP attempts never ban; TCP ones do, and the ban holds for both */
    cpace_guard_config(100U, 0U, 8U, 600U);
    cpace_guard_reset();
    for (i = 0; i < 40; ++i) {
        CHECK(cpace_guard_attempt(IP(10, 2, 0, 1), t + i, &banned, 0) ==
                  CPACE_GUARD_OK && !banned,
              "UDP attempt %d refused or banned", i);
    }
    /* 7 forged UDP ones before do not count toward a TCP row */
    for (i = 0; i < 7; ++i) {
        (void)cpace_guard_attempt(IP(10, 2, 0, 2), t, NULL, 0);
    }
    CHECK(cpace_guard_attempt(IP(10, 2, 0, 2), t + 1, &banned, 1) ==
              CPACE_GUARD_OK && !banned,
          "forged UDP attempts counted toward a TCP ban");
    for (i = 0; i < 7; ++i) {
        (void)cpace_guard_attempt(IP(10, 2, 0, 2), t + 2 + i, &banned, 1);
    }
    CHECK(banned, "no ban after 8 TCP attempts");
    CHECK(cpace_guard_attempt(IP(10, 2, 0, 2), t + 70, NULL, 0) ==
              CPACE_GUARD_BANNED,
          "a ban made over TCP lets a UDP attempt through");
    cpace_guard_confirm(IP(10, 2, 0, 2), t + 71);
    CHECK(cpace_guard_attempt(IP(10, 2, 0, 2), t + 72, NULL, 0) ==
              CPACE_GUARD_OK,
          "a confirm does not end the ban");

    /* #207: a flood of sources does not push a confirmed address out */
    cpace_guard_config(5U, 10U, 8U, 600U);
    cpace_guard_reset();
    (void)cpace_guard_attempt(IP(10, 3, 0, 1), t, NULL, 0);
    cpace_guard_confirm(IP(10, 3, 0, 1), t);
    for (i = 0; i < 5000; ++i) {
        (void)cpace_guard_attempt(IP(172, 17, (unsigned)(i >> 8),
                                     (unsigned)(i & 0xFF)), t + 1, NULL, 0);
    }
    for (i = 0; i < 4; ++i) {
        CHECK(cpace_guard_attempt(IP(10, 3, 0, 1), t + 2, NULL, 0) ==
                  CPACE_GUARD_OK,
              "a confirmed address held after a flood (%d)", i);
    }

    if (failures != 0) {
        fprintf(stderr, "test_cpaceguard: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_cpaceguard: OK\n");
    return 0;
}
