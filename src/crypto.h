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
#define CAMEX_PACKET_REGISTER 1U
#define CAMEX_PACKET_DATA     2U
#define CAMEX_PACKET_CONFIG   3U

/* Protocol version */
#define CAMEX_PROTO_VER 1U

typedef struct {
    uint8_t psk_key[32];
    uint8_t fingerprint[16];
    uint8_t ready;
} camex_crypto_t;

/* Key derivation from PSK string */
void derive_psk_key(const char *psk, uint8_t key[32]);

/* Init crypto context */
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

#endif /* CAMEX_CRYPTO_H */
