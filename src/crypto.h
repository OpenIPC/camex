/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * crypto.h — encryption / decryption, key derivation, replay protection
 *
 */

#ifndef CAMEX_CRYPTO_H
#define CAMEX_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

#include <stdlib.h>

/* crypto_wipe is defined in monocypher — forward-declare here */
void crypto_wipe(void *secret, size_t size);

#define CAMEX_HDR_LEN 27U
#define CAMEX_MAGIC "CX2P"

/* Packet types */
#define CAMEX_PACKET_REGISTER  1U
#define CAMEX_PACKET_DATA      2U
#define CAMEX_PACKET_CONFIG    3U
#define CAMEX_PACKET_KEEPALIVE 4U

/* Protocol version */
#define CAMEX_PROTO_VER 1U

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
 * Returns 0, -1 (malformed key) or KEY_ERR_NOMEM (no memory for
 * Argon2id: the key itself may be fine). */
int derive_psk_key(const char *psk, uint8_t key[32]);

/* Server: its static key pair from a base64 private key.
 * Client: the server's base64 public key. Return 0 or -1. */
int crypto_set_server_key(const char *priv_b64);
int crypto_set_server_pub(const char *pub_b64);

/* Init crypto context with the global key; on failure the context is left
 * as it was. Returns 0, -1 or KEY_ERR_NOMEM (see derive_psk_key()). */
int crypto_init(const char *psk);

/* AEAD encrypt (ChaCha20-Poly1305) */
int crypto_encrypt_packet(uint8_t type, uint64_t seq,
                          const uint8_t nonce_prefix[4],
                          const uint8_t *key,
                          const uint8_t *plain, size_t plain_len,
                          uint8_t *packet, size_t packet_size,
                          size_t *packet_len);

/* AEAD decrypt */
int crypto_decrypt_packet(const uint8_t *packet, size_t packet_len,
                          uint8_t *type,
                          uint8_t *plain, size_t plain_size,
                          size_t *plain_len, uint64_t *seq_out,
                          const uint8_t *key);

/* Replay protection (64-bit sliding window) */
int replay_check(uint64_t *seq_max, uint64_t *window, uint64_t seq);

/* Log the fingerprint */
void crypto_log_fingerprint(void);

/* Access to global crypto context (for fingerprint etc.) */
const camex_crypto_t *crypto_get_ctx(void);

/* crypto_wipe() is provided by monocypher.h */

void crypto_wipe_ctx(void);

/* Drop the global key but keep encryption working for per-client keys:
 * the key is wiped and packets without an explicit key are refused
 * (#16 L3). */
void crypto_drop_global(void);

/* A failed reload puts the global key back as it was */
void crypto_save_ctx(camex_crypto_t *out);
void crypto_restore_ctx(const camex_crypto_t *in);

#endif /* CAMEX_CRYPTO_H */
