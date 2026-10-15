/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * keys.h — key material for protocol v2 (#17, #14): pre-shared keys from
 * raw keys or passphrases (Argon2id), the server's static key pair
 *
 */

#ifndef CAMEX_KEYS_H
#define CAMEX_KEYS_H

#include <stdint.h>
#include <stddef.h>

#define KEY_LEN 32U
#define KEY_B64_LEN 44U   /* base64 of 32 bytes, with padding */

/* Argon2id cost for passphrases: 8 MiB, 3 passes, 1 lane */
#define KEY_ARGON2_BLOCKS 8192U
#define KEY_ARGON2_PASSES 3U
#define KEY_ARGON2_SALT   "camex-v2-psk-kdf"  /* 16 bytes, the same everywhere */

/* Standard base64 with padding. encode: out_size >= 4*ceil(len/3)+1.
 * decode: returns the decoded length, or -1 on malformed input. */
size_t key_base64_encode(const uint8_t *in, size_t len, char *out,
                         size_t out_size);
int key_base64_decode(const char *in, uint8_t *out, size_t out_size);

/*
 * A pre-shared key from its text form:
 *   "hex:<64 hex digits>" or "base64:<44 chars>" — a raw 32-byte key;
 *   anything else — a passphrase, stretched with Argon2id.
 * Argon2id results are cached by passphrase, so a config reload does not
 * recompute unchanged keys. Returns 0, or -1 (malformed raw key, no
 * memory).
 */
int key_from_psk(const char *psk, uint8_t key[KEY_LEN]);
void key_cache_clear(void);

/* 32-byte value in base64 (public key, private key) */
int key_parse_b64(const char *text, uint8_t key[KEY_LEN]);

/* Server key pair from its base64 private key */
int key_server_from_b64(const char *priv_b64, uint8_t priv[KEY_LEN],
                        uint8_t pub[KEY_LEN]);

/* A new random key pair, both in base64. Returns 0 or -1. */
int key_generate(char priv_b64[KEY_B64_LEN + 1U],
                 char pub_b64[KEY_B64_LEN + 1U]);

#endif /* CAMEX_KEYS_H */
