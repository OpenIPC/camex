/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * keys.c — key material for protocol v2 (#17, #14)
 *
 */

#include "keys.h"
#include "log.h"
#include "monocypher.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/mman.h>
#endif

static const char b64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t key_base64_encode(const uint8_t *in, size_t len, char *out,
                         size_t out_size)
{
    size_t i;
    size_t o = 0U;

    if (out_size < ((len + 2U) / 3U) * 4U + 1U) {
        return 0U;
    }
    for (i = 0U; i < len; i += 3U) {
        uint32_t v = (uint32_t)in[i] << 16;

        if (i + 1U < len) {
            v |= (uint32_t)in[i + 1U] << 8;
        }
        if (i + 2U < len) {
            v |= in[i + 2U];
        }
        out[o++] = b64_chars[(v >> 18) & 0x3fU];
        out[o++] = b64_chars[(v >> 12) & 0x3fU];
        out[o++] = (i + 1U < len) ? b64_chars[(v >> 6) & 0x3fU] : '=';
        out[o++] = (i + 2U < len) ? b64_chars[v & 0x3fU] : '=';
    }
    out[o] = '\0';
    return o;
}

static int b64_value(char c)
{
    const char *p;

    if (c == '\0') {
        return -1;
    }
    p = strchr(b64_chars, c);
    return p != NULL ? (int)(p - b64_chars) : -1;
}

int key_base64_decode(const char *in, uint8_t *out, size_t out_size)
{
    size_t len = strlen(in);
    size_t i;
    size_t o = 0U;

    if (len == 0U || len % 4U != 0U) {
        return -1;
    }
    for (i = 0U; i < len; i += 4U) {
        int a = b64_value(in[i]);
        int b = b64_value(in[i + 1U]);
        int c = (in[i + 2U] == '=') ? -2 : b64_value(in[i + 2U]);
        int d = (in[i + 3U] == '=') ? -2 : b64_value(in[i + 3U]);
        uint32_t v;

        if (a < 0 || b < 0 || c == -1 || d == -1 ||
            (c == -2 && d != -2) ||
            ((c == -2 || d == -2) && i + 4U != len)) {
            return -1;
        }
        v = ((uint32_t)a << 18) | ((uint32_t)b << 12) |
            ((uint32_t)(c < 0 ? 0 : c) << 6) | (uint32_t)(d < 0 ? 0 : d);
        if (o >= out_size) {
            return -1;
        }
        out[o++] = (uint8_t)(v >> 16);
        if (c >= 0) {
            if (o >= out_size) {
                return -1;
            }
            out[o++] = (uint8_t)(v >> 8);
        }
        if (d >= 0) {
            if (o >= out_size) {
                return -1;
            }
            out[o++] = (uint8_t)v;
        }
    }
    return (int)o;
}

int key_parse_b64(const char *text, uint8_t key[KEY_LEN])
{
    uint8_t buf[KEY_LEN + 3U];
    int n = key_base64_decode(text, buf, sizeof(buf));

    if (n != (int)KEY_LEN) {
        crypto_wipe(buf, sizeof(buf));
        return -1;
    }
    memcpy(key, buf, KEY_LEN);
    crypto_wipe(buf, sizeof(buf));
    return 0;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int parse_hex_key(const char *text, uint8_t key[KEY_LEN])
{
    size_t i;

    if (strlen(text) != KEY_LEN * 2U) {
        return -1;
    }
    for (i = 0U; i < KEY_LEN; ++i) {
        int hi = hex_value(text[2U * i]);
        int lo = hex_value(text[2U * i + 1U]);

        if (hi < 0 || lo < 0) {
            return -1;
        }
        key[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* Cache of Argon2id results, by BLAKE2b of the passphrase keyed with a
 * random per-process key (so the id is no fast hash of the passphrase) */
#define KEY_CACHE_SIZE 260U

typedef struct {
    uint8_t id[KEY_LEN];
    uint8_t key[KEY_LEN];
    uint8_t used;
    uint8_t seen;    /* looked up since key_cache_mark() */
} key_cache_entry_t;

static key_cache_entry_t key_cache[KEY_CACHE_SIZE];
static size_t key_cache_next = 0U;
static uint8_t key_cache_id_key[KEY_LEN];
static int key_cache_id_ready = 0;

void key_cache_clear(void)
{
    crypto_wipe(key_cache, sizeof(key_cache));
    crypto_wipe(key_cache_id_key, sizeof(key_cache_id_key));
    key_cache_id_ready = 0;
    key_cache_next = 0U;
}

void key_cache_mark(void)
{
    size_t i;

    for (i = 0U; i < KEY_CACHE_SIZE; ++i) {
        key_cache[i].seen = 0U;
    }
}

void key_cache_sweep(void)
{
    size_t i;

    for (i = 0U; i < KEY_CACHE_SIZE; ++i) {
        if (key_cache[i].used && !key_cache[i].seen) {
            crypto_wipe(&key_cache[i], sizeof(key_cache[i]));
        }
    }
}

/* Lock memory again after a temporary munlockall() */
static void relock_memory(int relock)
{
#ifndef _WIN32
    if (relock && mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        log_message(LOG_WARNING, "SECURITY WARNING: memory could not be "
                    "locked again after the passphrase key derivation");
    }
#else
    (void)relock;
#endif
}

static int argon2id(const char *pass, uint8_t key[KEY_LEN])
{
    crypto_argon2_config config;
    crypto_argon2_inputs inputs;
    void *work = malloc((size_t)KEY_ARGON2_BLOCKS * 1024U);
    int relock = 0;

#ifndef _WIN32
    /* With mlockall(MCL_FUTURE) and a small RLIMIT_MEMLOCK (containers,
     * non-root) the 8 MiB work area cannot be locked, so the allocation
     * fails. Unlock for the derivation (the area is wiped afterwards) and
     * lock again. */
    if (work == NULL && munlockall() == 0) {
        relock = 1;
        work = malloc((size_t)KEY_ARGON2_BLOCKS * 1024U);
    }
#endif
    if (work == NULL) {
        log_message(LOG_ERR, "Out of memory for the passphrase key");
        relock_memory(relock);
        return -1;
    }
    config.algorithm = CRYPTO_ARGON2_ID;
    config.nb_blocks = KEY_ARGON2_BLOCKS;
    config.nb_passes = KEY_ARGON2_PASSES;
    config.nb_lanes = 1U;
    inputs.pass = (const uint8_t *)pass;
    inputs.pass_size = (uint32_t)strlen(pass);
    inputs.salt = (const uint8_t *)KEY_ARGON2_SALT;
    inputs.salt_size = (uint32_t)(sizeof(KEY_ARGON2_SALT) - 1U);
    crypto_argon2(key, KEY_LEN, work, config, inputs,
                  crypto_argon2_no_extras);
    crypto_wipe(work, (size_t)KEY_ARGON2_BLOCKS * 1024U);
    free(work);
    relock_memory(relock);
    return 0;
}

int key_from_psk(const char *psk, uint8_t key[KEY_LEN])
{
    uint8_t id[KEY_LEN];
    size_t i;
    key_cache_entry_t *slot;

    if (psk == NULL || *psk == '\0') {
        return -1;
    }
    if (strncmp(psk, "hex:", 4) == 0) {
        if (parse_hex_key(psk + 4, key) != 0) {
            log_message(LOG_ERR, "Invalid hex: key (need 64 hex digits)");
            return -1;
        }
        return 0;
    }
    if (strncmp(psk, "base64:", 7) == 0) {
        if (key_parse_b64(psk + 7, key) != 0) {
            log_message(LOG_ERR, "Invalid base64: key (need 32 bytes)");
            return -1;
        }
        return 0;
    }

    /* A passphrase: Argon2id, cached (not cached without a random id
     * key) */
    if (!key_cache_id_ready &&
        get_random_bytes(key_cache_id_key, sizeof(key_cache_id_key)) == 0) {
        key_cache_id_ready = 1;
    }
    if (!key_cache_id_ready) {
        return argon2id(psk, key);
    }
    crypto_blake2b_keyed(id, sizeof(id), key_cache_id_key,
                         sizeof(key_cache_id_key),
                         (const uint8_t *)psk, strlen(psk));
    for (i = 0U; i < KEY_CACHE_SIZE; ++i) {
        if (key_cache[i].used && crypto_verify32(key_cache[i].id, id) == 0) {
            memcpy(key, key_cache[i].key, KEY_LEN);
            key_cache[i].seen = 1U;
            crypto_wipe(id, sizeof(id));
            return 0;
        }
    }
    if (argon2id(psk, key) != 0) {
        crypto_wipe(id, sizeof(id));
        return -1;
    }
    /* A free slot first, else round robin */
    slot = NULL;
    for (i = 0U; i < KEY_CACHE_SIZE && slot == NULL; ++i) {
        if (!key_cache[i].used) {
            slot = &key_cache[i];
        }
    }
    if (slot == NULL) {
        slot = &key_cache[key_cache_next];
        key_cache_next = (key_cache_next + 1U) % KEY_CACHE_SIZE;
    }
    memcpy(slot->id, id, KEY_LEN);
    memcpy(slot->key, key, KEY_LEN);
    slot->used = 1U;
    slot->seen = 1U;
    crypto_wipe(id, sizeof(id));
    return 0;
}

int key_server_from_b64(const char *priv_b64, uint8_t priv[KEY_LEN],
                        uint8_t pub[KEY_LEN])
{
    if (key_parse_b64(priv_b64, priv) != 0) {
        return -1;
    }
    crypto_x25519_public_key(pub, priv);
    return 0;
}

int key_generate(char priv_b64[KEY_B64_LEN + 1U],
                 char pub_b64[KEY_B64_LEN + 1U])
{
    uint8_t priv[KEY_LEN];
    uint8_t pub[KEY_LEN];

    if (get_random_bytes(priv, sizeof(priv)) != 0) {
        return -1;
    }
    crypto_x25519_public_key(pub, priv);
    key_base64_encode(priv, KEY_LEN, priv_b64, KEY_B64_LEN + 1U);
    key_base64_encode(pub, KEY_LEN, pub_b64, KEY_B64_LEN + 1U);
    crypto_wipe(priv, sizeof(priv));
    return 0;
}
