/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * cpaceguard.c — limits on CPace attempts (cpaceguard.h)
 *
 */

#include "cpaceguard.h"
#include "monocypher.h"
#include "util.h"

#include <string.h>

#define GUARD_SLOTS  256U
#define GUARD_PROBES 8U
#define GUARD_GOOD_SLOTS 256U
#define GUARD_GOOD_SECS 3600

/* Buckets count in sixtieths of an attempt: per_min of them come back
 * each second, so per_min attempts a minute */
typedef struct {
    uint32_t ip;
    uint8_t used;
    time_t seen;
    time_t filled;           /* last refill */
    unsigned long tokens;    /* sixtieths */
    unsigned int row;        /* attempts in a row without a confirm */
    time_t banned_until;
} guard_entry_t;

/* An address whose client confirmed within GUARD_GOOD_SECS: never banned,
 * not held by the total. Apart, so a flood of sources cannot evict it. */
typedef struct {
    uint32_t ip;
    time_t until;
} guard_good_t;

static guard_entry_t guard[GUARD_SLOTS];
static guard_good_t good_tab[GUARD_GOOD_SLOTS];
static uint8_t guard_key[16];
static int guard_key_ready = 0;
static unsigned int cfg_per_ip = CPACE_GUARD_PER_IP_DEFAULT;
static unsigned int cfg_total = CPACE_GUARD_TOTAL_DEFAULT;
static unsigned int cfg_ban_after = CPACE_GUARD_BAN_AFTER_DEFAULT;
static unsigned int cfg_ban_secs = CPACE_GUARD_BAN_SECS_DEFAULT;
static unsigned long total_tokens = (unsigned long)CPACE_GUARD_TOTAL_DEFAULT * 60UL;
static time_t total_filled = 0;
static cpace_guard_stats_t counts;

void cpace_guard_config(unsigned int per_ip, unsigned int total,
                        unsigned int ban_after, unsigned int ban_secs)
{
    cfg_per_ip = per_ip;
    cfg_total = total;
    cfg_ban_after = ban_after;
    cfg_ban_secs = ban_secs;
    if (total_tokens > (unsigned long)cfg_total * 60UL) {
        total_tokens = (unsigned long)cfg_total * 60UL;
    }
}

void cpace_guard_reset(void)
{
    memset(guard, 0, sizeof(guard));
    memset(good_tab, 0, sizeof(good_tab));
    memset(&counts, 0, sizeof(counts));
    total_tokens = (unsigned long)cfg_total * 60UL;
    total_filled = 0;
}

/* A bucket of per_min attempts a minute, filled up to now */
static void refill(unsigned long *tokens, time_t *filled, unsigned int per_min,
                   time_t now)
{
    unsigned long cap = (unsigned long)per_min * 60UL;

    if (*filled == 0 || now < *filled) {
        *tokens = cap;
    } else if (now > *filled) {
        unsigned long secs = (unsigned long)(now - *filled);

        *tokens = secs >= 60UL ? cap : *tokens + secs * per_min;
        if (*tokens > cap) {
            *tokens = cap;
        }
    }
    *filled = now;
}

static uint32_t guard_hash(uint32_t ip)
{
    uint8_t msg[4];
    uint8_t h[4];

    if (!guard_key_ready) {
        uint8_t fresh[sizeof(guard_key)];

        if (get_random_bytes(fresh, sizeof(fresh)) == 0) {
            memcpy(guard_key, fresh, sizeof(fresh));
            guard_key_ready = 1;
        }
        crypto_wipe(fresh, sizeof(fresh));
    }
    msg[0] = (uint8_t)(ip >> 24);
    msg[1] = (uint8_t)(ip >> 16);
    msg[2] = (uint8_t)(ip >> 8);
    msg[3] = (uint8_t)ip;
    crypto_blake2b_keyed(h, sizeof(h), guard_key, sizeof(guard_key), msg,
                         sizeof(msg));
    return ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) |
           ((uint32_t)h[2] << 8) | (uint32_t)h[3];
}

/* The entry of ip; create: take a free or the oldest slot, a live ban last */
static guard_entry_t *guard_find(uint32_t ip, time_t now, int create)
{
    uint32_t h = guard_hash(ip);
    guard_entry_t *victim = NULL;
    uint32_t p;

    for (p = 0U; p < GUARD_PROBES; ++p) {
        guard_entry_t *e = &guard[(h + p) % GUARD_SLOTS];

        if (e->used && e->ip == ip) {
            return e;
        }
        if (!create) {
            continue;
        }
        if (!e->used) {
            if (victim == NULL || victim->used) {
                victim = e;
            }
        } else if (victim == NULL ||
                   (victim->used &&
                    ((victim->banned_until > now) > (e->banned_until > now) ||
                     ((victim->banned_until > now) == (e->banned_until > now) &&
                      e->seen < victim->seen)))) {
            victim = e;
        }
    }
    if (victim != NULL) {
        memset(victim, 0, sizeof(*victim));
        victim->used = 1U;
        victim->ip = ip;
    }
    return victim;
}

/* Its entry in the good table (create: a free slot or the one that ends
 * first), or NULL */
static guard_good_t *good_find(uint32_t ip, time_t now, int create)
{
    uint32_t h = guard_hash(ip ^ 0x9E3779B9U);
    guard_good_t *victim = NULL;
    uint32_t p;

    for (p = 0U; p < GUARD_PROBES; ++p) {
        guard_good_t *g = &good_tab[(h + p) % GUARD_GOOD_SLOTS];

        if (g->until > now && g->ip == ip) {
            return g;
        }
        if (create && (victim == NULL || g->until < victim->until)) {
            victim = g;
        }
    }
    if (victim != NULL) {
        victim->ip = ip;
        victim->until = 0;
    }
    return victim;
}

int cpace_guard_attempt(uint32_t ip, time_t now, int *banned_now, int proven)
{
    guard_entry_t *e = guard_find(ip, now, 1);
    int good = good_find(ip, now, 0) != NULL;

    if (banned_now != NULL) {
        *banned_now = 0;
    }
    if (e == NULL) {
        counts.refused++;
        return CPACE_GUARD_IP_LIMIT;
    }
    e->seen = now;
    if (e->banned_until > now) {
        counts.refused++;
        return CPACE_GUARD_BANNED;
    }
    /* Only attempts over TCP, whose handshake proves the address, make a
     * row toward a ban: a forged UDP source would ban its victim (#207) */
    if (proven) {
        e->row++;
    }
    if (cfg_ban_after != 0U && cfg_ban_secs != 0U && !good && proven &&
        e->row >= cfg_ban_after) {
        e->row = 0U;
        e->banned_until = now + (time_t)cfg_ban_secs;
        counts.bans++;
        counts.refused++;
        if (banned_now != NULL) {
            *banned_now = 1;
        }
        return CPACE_GUARD_BANNED;
    }
    if (cfg_per_ip != 0U) {
        refill(&e->tokens, &e->filled, cfg_per_ip, now);
        if (e->tokens < 60UL) {
            counts.refused++;
            return CPACE_GUARD_IP_LIMIT;
        }
    }
    if (cfg_total != 0U && !good) {
        refill(&total_tokens, &total_filled, cfg_total, now);
        if (total_tokens < 60UL) {
            counts.refused++;
            return CPACE_GUARD_TOTAL_LIMIT;
        }
        total_tokens -= 60UL;
    }
    if (cfg_per_ip != 0U) {
        e->tokens -= 60UL;
    }
    counts.attempts++;
    return CPACE_GUARD_OK;
}

int cpace_guard_busy(time_t now)
{
    if (cfg_total == 0U) {
        return 0;
    }
    refill(&total_tokens, &total_filled, cfg_total, now);
    return total_tokens < (unsigned long)cfg_total * 30UL;
}

int cpace_guard_good(uint32_t ip, time_t now)
{
    return good_find(ip, now, 0) != NULL;
}

#define COOKIE_EPOCH 120

static uint8_t cookie_key[2][32];
static long cookie_epoch[2] = { -1L, -1L };

/* The secret of epoch ep (slot ep % 2), made when first asked for */
static const uint8_t *cookie_secret(long ep)
{
    int s = (int)(ep & 1L);

    if (cookie_epoch[s] != ep) {
        if (get_random_bytes(cookie_key[s], sizeof(cookie_key[s])) != 0) {
            return NULL;
        }
        cookie_epoch[s] = ep;
    }
    return cookie_key[s];
}

static int cookie_of(uint32_t ip, uint16_t port, long ep, uint8_t out[16])
{
    const uint8_t *k = cookie_secret(ep);
    uint8_t msg[6];

    if (k == NULL) {
        return -1;
    }
    msg[0] = (uint8_t)(ip >> 24);
    msg[1] = (uint8_t)(ip >> 16);
    msg[2] = (uint8_t)(ip >> 8);
    msg[3] = (uint8_t)ip;
    msg[4] = (uint8_t)(port >> 8);
    msg[5] = (uint8_t)port;
    crypto_blake2b_keyed(out, 16U, k, 32U, msg, sizeof(msg));
    return 0;
}

void cpace_cookie_make(uint32_t ip, uint16_t port, time_t now,
                       uint8_t out[16])
{
    if (cookie_of(ip, port, (long)(now / COOKIE_EPOCH), out) != 0) {
        memset(out, 0, 16U);
    }
}

int cpace_cookie_check(uint32_t ip, uint16_t port, time_t now,
                       const uint8_t cookie[16])
{
    long ep = (long)(now / COOKIE_EPOCH);
    uint8_t want[16];
    int good = 0;
    int k;

    for (k = 0; k < 2 && !good; ++k) {
        /* The last epoch's secret only while it is still the one kept */
        if (k == 1 && cookie_epoch[(ep - 1L) & 1L] != ep - 1L) {
            break;
        }
        if (cookie_of(ip, port, ep - (long)k, want) == 0 &&
            crypto_verify16(want, cookie) == 0) {
            good = 1;
        }
    }
    crypto_wipe(want, sizeof(want));
    return good;
}

void cpace_guard_confirm(uint32_t ip, time_t now)
{
    guard_entry_t *e = guard_find(ip, now, 1);
    guard_good_t *g = good_find(ip, now, 1);
    unsigned long cap;

    counts.confirmed++;
    if (g != NULL) {
        g->until = now + GUARD_GOOD_SECS;
    }
    if (cfg_total != 0U) {
        cap = (unsigned long)cfg_total * 60UL;
        total_tokens = total_tokens + 60UL > cap ? cap : total_tokens + 60UL;
    }
    if (e == NULL) {
        return;
    }
    e->seen = now;
    e->row = 0U;
    e->banned_until = 0;
    if (cfg_per_ip != 0U) {
        cap = (unsigned long)cfg_per_ip * 60UL;
        e->tokens = e->tokens + 60UL > cap ? cap : e->tokens + 60UL;
    }
}

void cpace_guard_stats(cpace_guard_stats_t *s, time_t now)
{
    size_t i;

    *s = counts;
    s->banned_now = 0U;
    for (i = 0U; i < GUARD_SLOTS; ++i) {
        if (guard[i].used && guard[i].banned_until > now) {
            s->banned_now++;
        }
    }
}
