/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * noise.c — Noise_NKpsk0_25519_ChaChaPoly_BLAKE2b on Monocypher (#17)
 *
 */

#include "noise.h"
#include "monocypher.h"

#include <string.h>

#define NOISE_BLOCKLEN 128U
#define NOISE_PROTOCOL "Noise_NKpsk0_25519_ChaChaPoly_BLAKE2b"

void noise_hmac(uint8_t out[NOISE_HASHLEN],
                const uint8_t *key, size_t key_len,
                const uint8_t *data, size_t data_len)
{
    uint8_t k[NOISE_BLOCKLEN];
    uint8_t pad[NOISE_BLOCKLEN];
    uint8_t inner[NOISE_HASHLEN];
    crypto_blake2b_ctx ctx;
    size_t i;

    memset(k, 0, sizeof(k));
    if (key_len > NOISE_BLOCKLEN) {
        crypto_blake2b(k, NOISE_HASHLEN, key, key_len);
    } else {
        memcpy(k, key, key_len);
    }

    for (i = 0U; i < NOISE_BLOCKLEN; ++i) {
        pad[i] = (uint8_t)(k[i] ^ 0x36U);
    }
    crypto_blake2b_init(&ctx, NOISE_HASHLEN);
    crypto_blake2b_update(&ctx, pad, sizeof(pad));
    crypto_blake2b_update(&ctx, data, data_len);
    crypto_blake2b_final(&ctx, inner);

    for (i = 0U; i < NOISE_BLOCKLEN; ++i) {
        pad[i] = (uint8_t)(k[i] ^ 0x5cU);
    }
    crypto_blake2b_init(&ctx, NOISE_HASHLEN);
    crypto_blake2b_update(&ctx, pad, sizeof(pad));
    crypto_blake2b_update(&ctx, inner, sizeof(inner));
    crypto_blake2b_final(&ctx, out);

    crypto_wipe(k, sizeof(k));
    crypto_wipe(pad, sizeof(pad));
    crypto_wipe(inner, sizeof(inner));
}

/* HKDF(ck, ikm) -> out1, out2[, out3] (Noise section 4.3) */
static void hkdf(const uint8_t ck[NOISE_HASHLEN],
                 const uint8_t *ikm, size_t ikm_len,
                 uint8_t out1[NOISE_HASHLEN], uint8_t out2[NOISE_HASHLEN],
                 uint8_t *out3)
{
    uint8_t temp_key[NOISE_HASHLEN];
    uint8_t buf[NOISE_HASHLEN + 1U];

    noise_hmac(temp_key, ck, NOISE_HASHLEN, ikm, ikm_len);
    buf[0] = 0x01U;
    noise_hmac(out1, temp_key, NOISE_HASHLEN, buf, 1U);
    memcpy(buf, out1, NOISE_HASHLEN);
    buf[NOISE_HASHLEN] = 0x02U;
    noise_hmac(out2, temp_key, NOISE_HASHLEN, buf, sizeof(buf));
    if (out3 != NULL) {
        memcpy(buf, out2, NOISE_HASHLEN);
        buf[NOISE_HASHLEN] = 0x03U;
        noise_hmac(out3, temp_key, NOISE_HASHLEN, buf, sizeof(buf));
    }
    crypto_wipe(temp_key, sizeof(temp_key));
    crypto_wipe(buf, sizeof(buf));
}

static void make_nonce(uint8_t nonce[12], uint64_t n)
{
    unsigned int i;

    memset(nonce, 0, 4);
    for (i = 0U; i < 8U; ++i) {
        nonce[4U + i] = (uint8_t)(n >> (8U * i));
    }
}

void noise_encrypt(const uint8_t key[NOISE_KEYLEN], uint64_t n,
                   const uint8_t *ad, size_t ad_len,
                   const uint8_t *plain, size_t len, uint8_t *out)
{
    crypto_aead_ctx ctx;
    uint8_t nonce[12];

    make_nonce(nonce, n);
    crypto_aead_init_ietf(&ctx, key, nonce);
    crypto_aead_write(&ctx, out, out + len, ad, ad_len, plain, len);
    crypto_wipe(&ctx, sizeof(ctx));
}

int noise_decrypt(const uint8_t key[NOISE_KEYLEN], uint64_t n,
                  const uint8_t *ad, size_t ad_len,
                  const uint8_t *in, size_t in_len, uint8_t *plain)
{
    crypto_aead_ctx ctx;
    uint8_t nonce[12];
    int rc;

    if (in_len < NOISE_TAGLEN) {
        return -1;
    }
    make_nonce(nonce, n);
    crypto_aead_init_ietf(&ctx, key, nonce);
    rc = crypto_aead_read(&ctx, plain, in + in_len - NOISE_TAGLEN, ad, ad_len,
                          in, in_len - NOISE_TAGLEN);
    crypto_wipe(&ctx, sizeof(ctx));
    return rc == 0 ? 0 : -1;
}

static void mix_hash(noise_hs_t *hs, const uint8_t *data, size_t len)
{
    crypto_blake2b_ctx ctx;

    crypto_blake2b_init(&ctx, NOISE_HASHLEN);
    crypto_blake2b_update(&ctx, hs->h, NOISE_HASHLEN);
    crypto_blake2b_update(&ctx, data, len);
    crypto_blake2b_final(&ctx, hs->h);
}

static void mix_key(noise_hs_t *hs, const uint8_t *ikm, size_t len)
{
    uint8_t temp_k[NOISE_HASHLEN];

    hkdf(hs->ck, ikm, len, hs->ck, temp_k, NULL);
    memcpy(hs->cs.k, temp_k, NOISE_KEYLEN);
    hs->cs.n = 0U;
    hs->cs.has_key = 1U;
    crypto_wipe(temp_k, sizeof(temp_k));
}

static void mix_key_and_hash(noise_hs_t *hs, const uint8_t *ikm, size_t len)
{
    uint8_t temp_h[NOISE_HASHLEN];
    uint8_t temp_k[NOISE_HASHLEN];

    hkdf(hs->ck, ikm, len, hs->ck, temp_h, temp_k);
    mix_hash(hs, temp_h, NOISE_HASHLEN);
    memcpy(hs->cs.k, temp_k, NOISE_KEYLEN);
    hs->cs.n = 0U;
    hs->cs.has_key = 1U;
    crypto_wipe(temp_h, sizeof(temp_h));
    crypto_wipe(temp_k, sizeof(temp_k));
}

/* EncryptAndHash: the key is always set in this pattern (psk0) */
static void encrypt_and_hash(noise_hs_t *hs, const uint8_t *plain,
                             size_t len, uint8_t *out)
{
    noise_encrypt(hs->cs.k, hs->cs.n, hs->h, NOISE_HASHLEN, plain, len, out);
    hs->cs.n++;
    mix_hash(hs, out, len + NOISE_TAGLEN);
}

static int decrypt_and_hash(noise_hs_t *hs, const uint8_t *in, size_t len,
                            uint8_t *plain)
{
    crypto_blake2b_ctx ctx;
    uint8_t next_h[NOISE_HASHLEN];

    /* The transcript takes the ciphertext: hash it before decrypting, in
     * case plain and in are the same buffer */
    crypto_blake2b_init(&ctx, NOISE_HASHLEN);
    crypto_blake2b_update(&ctx, hs->h, NOISE_HASHLEN);
    crypto_blake2b_update(&ctx, in, len);
    crypto_blake2b_final(&ctx, next_h);
    if (noise_decrypt(hs->cs.k, hs->cs.n, hs->h, NOISE_HASHLEN, in, len,
                      plain) != 0) {
        crypto_wipe(next_h, sizeof(next_h));
        return -1;
    }
    hs->cs.n++;
    memcpy(hs->h, next_h, NOISE_HASHLEN);
    return 0;
}

/* DH that rejects low-order points (all-zero shared secret) */
static int dh(uint8_t out[NOISE_DHLEN], const uint8_t priv[NOISE_DHLEN],
              const uint8_t pub[NOISE_DHLEN])
{
    static const uint8_t zero[NOISE_DHLEN] = { 0 };

    crypto_x25519(out, priv, pub);
    return crypto_verify32(out, zero) == 0 ? -1 : 0;
}

void noise_keypair(uint8_t priv[NOISE_DHLEN], uint8_t pub[NOISE_DHLEN],
                   const uint8_t seed[NOISE_DHLEN])
{
    memcpy(priv, seed, NOISE_DHLEN);
    crypto_x25519_public_key(pub, priv);
}

static void init_common(noise_hs_t *hs, const uint8_t *prologue,
                        size_t prologue_len,
                        const uint8_t server_pub[NOISE_DHLEN])
{
    memset(hs, 0, sizeof(*hs));
    /* Protocol name is shorter than HASHLEN: h = name padded with zeros */
    memcpy(hs->h, NOISE_PROTOCOL, sizeof(NOISE_PROTOCOL) - 1U);
    memcpy(hs->ck, hs->h, NOISE_HASHLEN);
    mix_hash(hs, prologue, prologue_len);
    /* Pre-message: <- s */
    mix_hash(hs, server_pub, NOISE_DHLEN);
}

void noise_init_initiator(noise_hs_t *hs, const uint8_t *prologue,
                          size_t prologue_len,
                          const uint8_t server_pub[NOISE_DHLEN])
{
    init_common(hs, prologue, prologue_len, server_pub);
    memcpy(hs->s_pub, server_pub, NOISE_DHLEN);
    hs->initiator = 1U;
}

void noise_init_responder(noise_hs_t *hs, const uint8_t *prologue,
                          size_t prologue_len,
                          const uint8_t s_priv[NOISE_DHLEN],
                          const uint8_t s_pub[NOISE_DHLEN])
{
    init_common(hs, prologue, prologue_len, s_pub);
    memcpy(hs->s_priv, s_priv, NOISE_DHLEN);
    memcpy(hs->s_pub, s_pub, NOISE_DHLEN);
    hs->initiator = 0U;
}

size_t noise_write_msg1(noise_hs_t *hs, const uint8_t psk[NOISE_KEYLEN],
                        const uint8_t e_seed[NOISE_DHLEN],
                        const uint8_t *payload, size_t payload_len,
                        uint8_t *out, size_t out_size)
{
    uint8_t shared[NOISE_DHLEN];
    size_t len;

    if (!hs->initiator || out_size < NOISE_MSG1_OVERHEAD ||
        payload_len > out_size - NOISE_MSG1_OVERHEAD) {
        return 0U;
    }
    len = NOISE_MSG1_OVERHEAD + payload_len;
    mix_key_and_hash(hs, psk, NOISE_KEYLEN);              /* psk */
    noise_keypair(hs->e_priv, hs->e_pub, e_seed);         /* e */
    memcpy(out, hs->e_pub, NOISE_DHLEN);
    mix_hash(hs, hs->e_pub, NOISE_DHLEN);
    mix_key(hs, hs->e_pub, NOISE_DHLEN);                  /* psk mode */
    if (dh(shared, hs->e_priv, hs->s_pub) != 0) {         /* es */
        crypto_wipe(shared, sizeof(shared));
        return 0U;
    }
    mix_key(hs, shared, NOISE_DHLEN);
    crypto_wipe(shared, sizeof(shared));
    encrypt_and_hash(hs, payload, payload_len, out + NOISE_DHLEN);
    return len;
}

int noise_msg1_es(const noise_hs_t *hs, const uint8_t *msg, size_t len,
                  uint8_t es[NOISE_DHLEN])
{
    if (hs->initiator || len < NOISE_MSG1_OVERHEAD) {
        return -1;
    }
    return dh(es, hs->s_priv, msg);
}

static int read_msg1(noise_hs_t *hs, const uint8_t psk[NOISE_KEYLEN],
                    const uint8_t es[NOISE_DHLEN],
                    const uint8_t *msg, size_t len,
                    uint8_t *payload, size_t payload_size,
                    size_t *payload_len)
{
    size_t plen;

    if (hs->initiator || len < NOISE_MSG1_OVERHEAD) {
        return -1;
    }
    plen = len - NOISE_MSG1_OVERHEAD;
    if (payload_size < plen) {
        return -1;
    }
    mix_key_and_hash(hs, psk, NOISE_KEYLEN);              /* psk */
    memcpy(hs->re, msg, NOISE_DHLEN);                     /* e */
    mix_hash(hs, hs->re, NOISE_DHLEN);
    mix_key(hs, hs->re, NOISE_DHLEN);
    mix_key(hs, es, NOISE_DHLEN);                         /* es */
    if (decrypt_and_hash(hs, msg + NOISE_DHLEN, len - NOISE_DHLEN,
                         payload) != 0) {
        return -1;
    }
    *payload_len = plen;
    return 0;
}

size_t noise_write_msg2(noise_hs_t *hs, const uint8_t e_seed[NOISE_DHLEN],
                        const uint8_t *payload, size_t payload_len,
                        uint8_t *out, size_t out_size)
{
    uint8_t shared[NOISE_DHLEN];
    size_t len;

    if (hs->initiator || out_size < NOISE_MSG2_OVERHEAD ||
        payload_len > out_size - NOISE_MSG2_OVERHEAD) {
        return 0U;
    }
    len = NOISE_MSG2_OVERHEAD + payload_len;
    noise_keypair(hs->e_priv, hs->e_pub, e_seed);         /* e */
    memcpy(out, hs->e_pub, NOISE_DHLEN);
    mix_hash(hs, hs->e_pub, NOISE_DHLEN);
    mix_key(hs, hs->e_pub, NOISE_DHLEN);
    if (dh(shared, hs->e_priv, hs->re) != 0) {            /* ee */
        crypto_wipe(shared, sizeof(shared));
        return 0U;
    }
    mix_key(hs, shared, NOISE_DHLEN);
    crypto_wipe(shared, sizeof(shared));
    encrypt_and_hash(hs, payload, payload_len, out + NOISE_DHLEN);
    return len;
}

static int read_msg2(noise_hs_t *hs, const uint8_t *msg, size_t len,
                    uint8_t *payload, size_t payload_size,
                    size_t *payload_len)
{
    uint8_t shared[NOISE_DHLEN];
    size_t plen;

    if (!hs->initiator || len < NOISE_MSG2_OVERHEAD) {
        return -1;
    }
    plen = len - NOISE_MSG2_OVERHEAD;
    if (payload_size < plen) {
        return -1;
    }
    memcpy(hs->re, msg, NOISE_DHLEN);                     /* e */
    mix_hash(hs, hs->re, NOISE_DHLEN);
    mix_key(hs, hs->re, NOISE_DHLEN);
    if (dh(shared, hs->e_priv, hs->re) != 0) {            /* ee */
        crypto_wipe(shared, sizeof(shared));
        return -1;
    }
    mix_key(hs, shared, NOISE_DHLEN);
    crypto_wipe(shared, sizeof(shared));
    if (decrypt_and_hash(hs, msg + NOISE_DHLEN, len - NOISE_DHLEN,
                         payload) != 0) {
        return -1;
    }
    *payload_len = plen;
    return 0;
}

/* The read functions work on a copy: a message that fails leaves the
 * handshake state as it was, so a valid message can still follow */
int noise_read_msg1(noise_hs_t *hs, const uint8_t psk[NOISE_KEYLEN],
                    const uint8_t es[NOISE_DHLEN],
                    const uint8_t *msg, size_t len,
                    uint8_t *payload, size_t payload_size,
                    size_t *payload_len)
{
    noise_hs_t t = *hs;
    int rc = read_msg1(&t, psk, es, msg, len, payload, payload_size,
                       payload_len);

    if (rc == 0) {
        *hs = t;
    }
    noise_wipe(&t);
    return rc;
}

int noise_read_msg2(noise_hs_t *hs, const uint8_t *msg, size_t len,
                    uint8_t *payload, size_t payload_size,
                    size_t *payload_len)
{
    noise_hs_t t = *hs;
    int rc = read_msg2(&t, msg, len, payload, payload_size, payload_len);

    if (rc == 0) {
        *hs = t;
    }
    noise_wipe(&t);
    return rc;
}

void noise_split(noise_hs_t *hs, uint8_t c1[NOISE_KEYLEN],
                 uint8_t c2[NOISE_KEYLEN])
{
    uint8_t k1[NOISE_HASHLEN];
    uint8_t k2[NOISE_HASHLEN];

    hkdf(hs->ck, NULL, 0U, k1, k2, NULL);
    memcpy(c1, k1, NOISE_KEYLEN);
    memcpy(c2, k2, NOISE_KEYLEN);
    crypto_wipe(k1, sizeof(k1));
    crypto_wipe(k2, sizeof(k2));
}

void noise_wipe(noise_hs_t *hs)
{
    crypto_wipe(hs, sizeof(*hs));
}
