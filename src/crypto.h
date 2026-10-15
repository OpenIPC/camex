/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * crypto.h — key context: global PSK, server key pair (protocol v2)
 *
 */

#ifndef CAMEX_CRYPTO_H
#define CAMEX_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

#include <stdlib.h>

/* crypto_wipe is defined in monocypher — forward-declare here */
void crypto_wipe(void *secret, size_t size);

#include "session.h"   /* wire format: CAMEX_MAGIC, CAMEX_PROTO_VER */

typedef struct {
    uint8_t psk_key[32];
    uint8_t fingerprint[16];
    uint8_t ready;
    uint8_t has_global;   /* psk_key is a usable global key */
    uint8_t server_priv[32];  /* server: its static key pair (v2) */
    uint8_t server_pub[32];   /* server: own; client: the server's */
    uint8_t has_server_key;   /* server_priv is set */
    uint8_t has_server_pub;   /* server_pub is set */
} camex_crypto_t;

/* Key from a PSK string: hex:/base64: raw key, or Argon2id (keys.h).
 * Returns 0 or -1. */
int derive_psk_key(const char *psk, uint8_t key[32]);

/* Server: its static key pair from a base64 private key.
 * Client: the server's base64 public key. Return 0 or -1. */
int crypto_set_server_key(const char *priv_b64);
int crypto_set_server_pub(const char *pub_b64);

/* Init the key context. psk may be empty on a server whose clients all
 * have their own keys: there is no global key then. */
int crypto_init(const char *psk);

/* Log the fingerprint */
void crypto_log_fingerprint(void);

/* Access to global crypto context (for fingerprint etc.) */
const camex_crypto_t *crypto_get_ctx(void);

/* crypto_wipe() is provided by monocypher.h */

void crypto_wipe_ctx(void);

/* Drop the global key (a reload removed it); per-client keys stay
 * (#16 L3). */
void crypto_drop_global(void);

/* A failed reload puts the global key back as it was */
void crypto_save_ctx(camex_crypto_t *out);
void crypto_restore_ctx(const camex_crypto_t *in);

#endif /* CAMEX_CRYPTO_H */
