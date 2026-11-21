/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * cpaceguard.h — limits on CPace attempts (named networks): per source
 * address, in all, and a ban for a source that keeps guessing
 *
 */

#ifndef CAMEX_CPACEGUARD_H
#define CAMEX_CPACEGUARD_H

#include <stdint.h>
#include <time.h>

/*
 * A wrong network name is found out by the client alone: it opens none of
 * PAKE2's tags and sends no PAKE3. So the server counts a PAKE1 as one
 * guess, and a PAKE3 that confirms gives it back. A real client confirms
 * at once and spends nothing; a guesser spends one per name tried, against
 * every network the server lists.
 *
 *  - per source address: at most per_ip unconfirmed attempts a minute
 *    (a bucket of per_ip, filled again at per_ip a minute);
 *  - in all: at most total unconfirmed attempts a minute, except for an
 *    address whose client confirmed within the last hour;
 *  - ban: ban_after attempts in a row from an address without one that
 *    confirmed refuse it for ban_secs seconds; an address with a client
 *    that confirmed within the hour (a NAT with real cameras behind it)
 *    is limited, never banned.
 * A limit of 0 turns it off. Sources live in a fixed table placed by a
 * keyed hash: a flood of addresses evicts the oldest entries, never the
 * live bans first.
 */

#define CPACE_GUARD_PER_IP_DEFAULT   20U
#define CPACE_GUARD_TOTAL_DEFAULT    300U
#define CPACE_GUARD_BAN_AFTER_DEFAULT 30U
#define CPACE_GUARD_BAN_SECS_DEFAULT 600U

enum {
    CPACE_GUARD_OK = 0,
    CPACE_GUARD_BANNED = 1,       /* the source is banned */
    CPACE_GUARD_IP_LIMIT = 2,     /* this source's attempts a minute */
    CPACE_GUARD_TOTAL_LIMIT = 3   /* all sources' attempts a minute */
};

typedef struct {
    unsigned long attempts;    /* PAKE1 taken */
    unsigned long confirmed;   /* PAKE3 that confirmed */
    unsigned long refused;     /* PAKE1 refused by a limit or a ban */
    unsigned long bans;        /* bans given */
    unsigned int banned_now;   /* sources banned at this moment */
} cpace_guard_stats_t;

/* The limits (0: off); also on a reload */
void cpace_guard_config(unsigned int per_ip, unsigned int total,
                        unsigned int ban_after, unsigned int ban_secs);

/* A PAKE1 from ip (IPv4, host order) at now: CPACE_GUARD_OK or why it is
 * refused. *banned_now is 1 when this attempt made the ban (for the log) */
int cpace_guard_attempt(uint32_t ip, time_t now, int *banned_now);

/* A PAKE3 from ip confirmed: its attempt is given back, its row of
 * attempts ends */
void cpace_guard_confirm(uint32_t ip, time_t now);

void cpace_guard_stats(cpace_guard_stats_t *s, time_t now);

/* Forget everything (tests) */
void cpace_guard_reset(void);

#endif /* CAMEX_CPACEGUARD_H */
