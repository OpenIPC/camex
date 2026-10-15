/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * noise.h — Noise_NKpsk0_25519_ChaChaPoly_BLAKE2b (protocol v2, #17)
 *
 *   <- s
 *   ...
 *   -> psk, e, es
 *   <- e, ee
 *
 * The client (initiator) knows the server's static public key; both sides
 * know the client's pre-shared key. See https://noiseprotocol.org/noise.html
 *
 */

#ifndef CAMEX_NOISE_H
#define CAMEX_NOISE_H

#include <stdint.h>
#include <stddef.h>

#define NOISE_DHLEN   32U
#define NOISE_HASHLEN 64U
#define NOISE_KEYLEN  32U
#define NOISE_TAGLEN  16U

/* Message sizes without payload */
#define NOISE_MSG1_OVERHEAD (NOISE_DHLEN + NOISE_TAGLEN)
#define NOISE_MSG2_OVERHEAD (NOISE_DHLEN + NOISE_TAGLEN)

typedef struct {
    uint8_t k[NOISE_KEYLEN];
    uint64_t n;
    uint8_t has_key;
} noise_cipher_t;

typedef struct {
    noise_cipher_t cs;
    uint8_t ck[NOISE_HASHLEN];
    uint8_t h[NOISE_HASHLEN];
    uint8_t s_priv[NOISE_DHLEN];   /* responder: static key pair */
    uint8_t s_pub[NOISE_DHLEN];    /* responder: own; initiator: remote */
    uint8_t e_priv[NOISE_DHLEN];
    uint8_t e_pub[NOISE_DHLEN];
    uint8_t re[NOISE_DHLEN];       /* remote ephemeral */
    uint8_t initiator;
} noise_hs_t;

/* HMAC-BLAKE2b (64-byte output), as Noise's HASH = BLAKE2b */
void noise_hmac(uint8_t out[NOISE_HASHLEN],
                const uint8_t *key, size_t key_len,
                const uint8_t *data, size_t data_len);

/* Key pair from 32 random (or given) bytes */
void noise_keypair(uint8_t priv[NOISE_DHLEN], uint8_t pub[NOISE_DHLEN],
                   const uint8_t seed[NOISE_DHLEN]);

/*
 * Initiator (client): server_pub is the server's static public key.
 * Responder (server): its static key pair.
 */
void noise_init_initiator(noise_hs_t *hs, const uint8_t *prologue,
                          size_t prologue_len,
                          const uint8_t server_pub[NOISE_DHLEN]);
void noise_init_responder(noise_hs_t *hs, const uint8_t *prologue,
                          size_t prologue_len,
                          const uint8_t s_priv[NOISE_DHLEN],
                          const uint8_t s_pub[NOISE_DHLEN]);

/*
 * Message 1 (client -> server): e.pub || AEAD(payload). e_seed is 32
 * random bytes for the ephemeral key (fixed in tests).
 * Returns the message length, or 0 on error.
 */
size_t noise_write_msg1(noise_hs_t *hs, const uint8_t psk[NOISE_KEYLEN],
                        const uint8_t e_seed[NOISE_DHLEN],
                        const uint8_t *payload, size_t payload_len,
                        uint8_t *out, size_t out_size);

/*
 * Message 1 on the server. The ephemeral DH (es) does not depend on the
 * PSK: noise_msg1_es() computes it once, then noise_read_msg1() can be
 * tried with each candidate PSK (a failed read leaves the state as it
 * was). Return 0 on success, -1 on failure. Message 1 replay protection
 * (e.g. a timestamp in the payload) is the caller's job.
 */
int noise_msg1_es(const noise_hs_t *hs, const uint8_t *msg, size_t len,
                  uint8_t es[NOISE_DHLEN]);
int noise_read_msg1(noise_hs_t *hs, const uint8_t psk[NOISE_KEYLEN],
                    const uint8_t es[NOISE_DHLEN],
                    const uint8_t *msg, size_t len,
                    uint8_t *payload, size_t payload_size,
                    size_t *payload_len);

/* Message 2 (server -> client): e.pub || AEAD(payload). A failed read
 * leaves the state as it was, so a valid reply can still be processed. */
size_t noise_write_msg2(noise_hs_t *hs, const uint8_t e_seed[NOISE_DHLEN],
                        const uint8_t *payload, size_t payload_len,
                        uint8_t *out, size_t out_size);
int noise_read_msg2(noise_hs_t *hs, const uint8_t *msg, size_t len,
                    uint8_t *payload, size_t payload_size,
                    size_t *payload_len);

/* After message 2: c1 = initiator -> responder, c2 = responder ->
 * initiator. Call noise_wipe() on the handshake state afterwards. */
void noise_split(noise_hs_t *hs, uint8_t c1[NOISE_KEYLEN],
                 uint8_t c2[NOISE_KEYLEN]);

/* Transport: ChaCha20-Poly1305 with the 64-bit counter as the nonce
 * (Noise: 32 zero bits || little-endian counter). out = cipher || tag. */
void noise_encrypt(const uint8_t key[NOISE_KEYLEN], uint64_t n,
                   const uint8_t *ad, size_t ad_len,
                   const uint8_t *plain, size_t len, uint8_t *out);
int noise_decrypt(const uint8_t key[NOISE_KEYLEN], uint64_t n,
                  const uint8_t *ad, size_t ad_len,
                  const uint8_t *in, size_t in_len, uint8_t *plain);

void noise_wipe(noise_hs_t *hs);

#endif /* CAMEX_NOISE_H */
