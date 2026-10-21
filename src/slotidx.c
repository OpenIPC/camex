/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * slotidx.c — hash index from a key to slot numbers of an array (#75)
 *
 * Open addressing with linear probing, at most half full. Buckets are never
 * deleted: stale ones are skipped by match() and dropped when the owner
 * rebuilds the index (slotidx_crowded()).
 *
 */

#include "slotidx.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define SLOTIDX_MIN_BUCKETS 16U

static uint32_t buckets_for(size_t entries)
{
    uint32_t n = SLOTIDX_MIN_BUCKETS;

    /* At most half full; entries never exceed the 65534 slots of #75, so
     * this cannot overflow */
    while ((size_t)n < entries * 2U && n < 0x40000000U) {
        n <<= 1;
    }
    return n;
}

void slotidx_free(slotidx_t *x)
{
    if (x == NULL) {
        return;
    }
    free(x->hash);
    free(x->slot);
    memset(x, 0, sizeof(*x));
}

static int alloc_table(slotidx_t *x, uint32_t n)
{
    x->hash = calloc(n, sizeof(*x->hash));
    x->slot = calloc(n, sizeof(*x->slot));
    if (x->hash == NULL || x->slot == NULL) {
        slotidx_free(x);
        return -1;
    }
    x->mask = n - 1U;
    x->used = 0U;
    return 0;
}

int slotidx_reset(slotidx_t *x, size_t entries)
{
    uint32_t n = buckets_for(entries);

    if (x->mask != 0U && x->mask + 1U == n) {
        memset(x->hash, 0, (size_t)n * sizeof(*x->hash));
        memset(x->slot, 0, (size_t)n * sizeof(*x->slot));
        x->used = 0U;
        return 0;
    }
    slotidx_free(x);
    return alloc_table(x, n);
}

static void put(slotidx_t *x, uint32_t h, uint32_t slot1)
{
    uint32_t i = h & x->mask;

    while (x->slot[i] != 0U) {
        if (x->slot[i] == slot1 && x->hash[i] == h) {
            return;   /* already there */
        }
        i = (i + 1U) & x->mask;
    }
    x->hash[i] = h;
    x->slot[i] = slot1;
    x->used++;
}

int slotidx_insert(slotidx_t *x, uint32_t h, uint32_t slot)
{
    if (x->mask == 0U || (x->used + 1U) * 2U > x->mask + 1U) {
        slotidx_t bigger;
        uint32_t i;

        memset(&bigger, 0, sizeof(bigger));
        if (alloc_table(&bigger,
                        x->mask == 0U ? SLOTIDX_MIN_BUCKETS
                                      : (x->mask + 1U) * 2U) != 0) {
            return -1;
        }
        for (i = 0U; x->mask != 0U && i <= x->mask; ++i) {
            if (x->slot[i] != 0U) {
                put(&bigger, x->hash[i], x->slot[i]);
            }
        }
        slotidx_free(x);
        *x = bigger;
    }
    put(x, h, slot + 1U);
    return 0;
}

int slotidx_crowded(const slotidx_t *x, size_t live)
{
    return (size_t)x->used > live * 2U + SLOTIDX_MIN_BUCKETS;
}

long slotidx_find(const slotidx_t *x, uint32_t h,
                  slotidx_match_fn match, const void *key)
{
    uint32_t i;
    long best = -1;

    if (x == NULL || x->mask == 0U) {
        return -1;
    }
    for (i = h & x->mask; x->slot[i] != 0U; i = (i + 1U) & x->mask) {
        uint32_t slot = x->slot[i] - 1U;

        if (x->hash[i] == h && (best < 0 || (long)slot < best) &&
            match(slot, key)) {
            best = (long)slot;
        }
    }
    return best;
}

/* Finaliser of murmur3: spreads small integers (addresses of one subnet)
 * over all bits */
uint32_t slotidx_hash_u32(uint32_t v)
{
    v ^= v >> 16;
    v *= 0x85ebca6bU;
    v ^= v >> 13;
    v *= 0xc2b2ae35U;
    v ^= v >> 16;
    return v;
}

/* FNV-1a */
uint32_t slotidx_hash_bytes(const void *data, size_t len)
{
    const unsigned char *p = data;
    uint32_t h = 2166136261U;
    size_t i;

    for (i = 0U; i < len; ++i) {
        h ^= p[i];
        h *= 16777619U;
    }
    return slotidx_hash_u32(h);
}

uint32_t slotidx_hash_nocase(const char *s)
{
    uint32_t h = 2166136261U;

    while (s != NULL && *s != '\0') {
        h ^= (uint32_t)tolower((unsigned char)*s++);
        h *= 16777619U;
    }
    return slotidx_hash_u32(h);
}
