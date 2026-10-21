/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * slotidx.h — hash index from a key to slot numbers of an array (#75)
 *
 * The index stores only (hash, slot) pairs; the caller's match() callback
 * compares the real key with the slot, so an entry that went stale (the
 * slot was freed or got another key) is simply skipped. A lookup returns
 * the lowest matching slot, the same one a linear scan would find.
 *
 */

#ifndef CAMEX_SLOTIDX_H
#define CAMEX_SLOTIDX_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t *hash;   /* bucket hashes */
    uint32_t *slot;   /* slot + 1; 0 = empty bucket */
    uint32_t mask;    /* bucket count - 1 (a power of two), 0 = no table */
    uint32_t used;    /* occupied buckets, stale ones included */
} slotidx_t;

/* 1 if slot holds key */
typedef int (*slotidx_match_fn)(uint32_t slot, const void *key);

/* Empty the index and size it for about `entries` keys; 0 or -1 (no
 * memory: the index is then empty and slotidx_find() finds nothing) */
int slotidx_reset(slotidx_t *x, size_t entries);

/* Free the table */
void slotidx_free(slotidx_t *x);

/* Add slot under hash h; the table doubles when it gets half full.
 * 0, or -1 when there is no memory for that. */
int slotidx_insert(slotidx_t *x, uint32_t h, uint32_t slot);

/* 1 when stale entries fill the table: the caller should rebuild it */
int slotidx_crowded(const slotidx_t *x, size_t live);

/* Lowest slot under hash h that match() accepts, or -1 */
long slotidx_find(const slotidx_t *x, uint32_t h,
                  slotidx_match_fn match, const void *key);

/* Hashes of the keys the server indexes */
uint32_t slotidx_hash_u32(uint32_t v);
uint32_t slotidx_hash_bytes(const void *data, size_t len);
uint32_t slotidx_hash_nocase(const char *s);   /* case-insensitive */

#endif /* CAMEX_SLOTIDX_H */
