/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * crypto.c — encryption / decryption, key derivation, replay protection
 *
 */

#include "crypto.h"
#include "keys.h"
#include "log.h"
#include "util.h"
#include "monocypher.h"

#include <string.h>
#ifndef _WIN32
#include <arpa/inet.h>
#else
#include <winsock2.h>
#endif

static camex_crypto_t crypto_ctx;

int derive_psk_key(const char *psk, uint8_t key[32])
{
    if (psk == NULL) {
        memset(key, 0, 32);
        return -1;
    }
    return key_from_psk(psk, key);
}

int crypto_set_server_key(const char *priv_b64)
{
    char pub[KEY_B64_LEN + 1U];

    if (key_server_from_b64(priv_b64, crypto_ctx.server_priv,
                            crypto_ctx.server_pub) != 0) {
        log_message(LOG_ERR, "Invalid server key (need base64 of 32 bytes)");
        return -1;
    }
    crypto_ctx.has_server_key = 1U;
    crypto_ctx.has_server_pub = 1U;
    key_base64_encode(crypto_ctx.server_pub, KEY_LEN, pub, sizeof(pub));
    log_message(LOG_INFO, "Server public key: %s", pub);
    return 0;
}

int crypto_set_server_pub(const char *pub_b64)
{
    if (key_parse_b64(pub_b64, crypto_ctx.server_pub) != 0) {
        log_message(LOG_ERR,
                    "Invalid server public key (need base64 of 32 bytes)");
        return -1;
    }
    crypto_ctx.has_server_pub = 1U;
    return 0;
}

void crypto_log_fingerprint(void)
{
    char fp[33];
    hex_encode(crypto_ctx.fingerprint, sizeof(crypto_ctx.fingerprint),
               fp, sizeof(fp));
    log_message(LOG_INFO, "Encryption fingerprint: %s", fp);
}

const camex_crypto_t *crypto_get_ctx(void)
{
    return &crypto_ctx;
}

int crypto_init(const char *psk)
{
    if (psk == NULL || *psk == '\0') {
        log_message(LOG_ERR, "Encryption requested but PSK is empty");
        return -1;
    }

    {
        /* A bad key must not leave a partly overwritten key behind */
        uint8_t key[32];

        int rc = derive_psk_key(psk, key);

        if (rc != 0) {
            crypto_wipe(key, sizeof(key));
            return rc;   /* -1, or KEY_ERR_NOMEM */
        }
        memcpy(crypto_ctx.psk_key, key, sizeof(key));
        crypto_wipe(key, sizeof(key));
    }
    crypto_blake2b(crypto_ctx.fingerprint, sizeof(crypto_ctx.fingerprint),
                   crypto_ctx.psk_key, sizeof(crypto_ctx.psk_key));
    crypto_ctx.ready = 1U;
    crypto_ctx.has_global = 1U;
    crypto_log_fingerprint();
    return 0;
}

void crypto_drop_global(void)
{
    /* Packets without an explicit key used the global one: they are
     * refused from now on (see has_global checks below) */
    crypto_wipe(crypto_ctx.psk_key, sizeof(crypto_ctx.psk_key));
    crypto_ctx.has_global = 0U;
}

void crypto_wipe_ctx(void)
{
    crypto_wipe(&crypto_ctx, sizeof(crypto_ctx));
    crypto_ctx.ready = 0U;
}

int crypto_encrypt_packet(uint8_t type, uint64_t seq,
                          const uint8_t nonce_prefix[4],
                          const uint8_t *key,
                          const uint8_t *plain, size_t plain_len,
                          uint8_t *packet, size_t packet_size,
                          size_t *packet_len)
{
    crypto_aead_ctx ctx;
    uint8_t nonce[12];
    uint16_t body_len;

    if (key == NULL && !crypto_ctx.has_global) {
        return -1;  /* the global key was removed by a reload */
    }
    if (!crypto_ctx.ready || packet == NULL || packet_len == NULL ||
        (plain_len > 0U && plain == NULL)) {
        return -1;
    }

    if (plain_len > 65535U || packet_size < CAMEX_HDR_LEN + 1U + plain_len + 16U) {
        return -1;
    }

    memcpy(nonce, nonce_prefix, 4);
    write_be64(nonce + 4, seq);

    memcpy(packet, CAMEX_MAGIC, 4);
    packet[4] = type;

    /* Version byte appended at offset CAMEX_HDR_LEN (byte 27).
     * Old format (CAMEX_HDR_LEN=27): magic(4) type(1) nonce(12) body_len(2) seq(8)
     * New format (CAMEX_HDR_LEN+1=28): ... + ver(1) at offset 27.
     * The decryption side detects the version byte by packet length. */
    memcpy(packet + 5, nonce, 12);
    body_len = htons((uint16_t)plain_len);
    memcpy(packet + 17, &body_len, sizeof(body_len));
    write_be64(packet + 19, seq);
    /* Version byte at offset 27 */
    packet[27] = CAMEX_PROTO_VER;

    crypto_aead_init_ietf(&ctx, key != NULL ? key : crypto_ctx.psk_key, nonce);
    crypto_aead_write(&ctx, packet + CAMEX_HDR_LEN + 1,  /* +1 for version byte */
                      packet + CAMEX_HDR_LEN + 1 + plain_len,
                      packet, CAMEX_HDR_LEN + 1,  /* AD includes version byte */
                      plain, plain_len);
    *packet_len = CAMEX_HDR_LEN + 1 + plain_len + 16U;
    crypto_wipe(nonce, sizeof(nonce));
    return 0;
}

int crypto_decrypt_packet(const uint8_t *packet, size_t packet_len,
                          uint8_t *type,
                          uint8_t *plain, size_t plain_size,
                          size_t *plain_len, uint64_t *seq_out,
                          const uint8_t *key)
{
    crypto_aead_ctx ctx;
    uint8_t nonce[12];
    uint16_t body_len_be;
    size_t body_len;
    size_t hdr_size;

    if (key == NULL && !crypto_ctx.has_global) {
        return -1;  /* the global key was removed by a reload */
    }
    if (!crypto_ctx.ready || packet == NULL || type == NULL ||
        plain_len == NULL) {
        return -1;
    }

    if (packet_len < CAMEX_HDR_LEN + 1 + 16U) {
        if (packet_len < CAMEX_HDR_LEN + 16U) {
            return -1;
        }
        /* Old format (no version byte): use CAMEX_HDR_LEN = 27 */
        hdr_size = CAMEX_HDR_LEN;
    } else {
        /* Check version byte at CAMEX_HDR_LEN (offset 27) */
        if (packet[CAMEX_HDR_LEN] <= CAMEX_PROTO_VER) {
            hdr_size = CAMEX_HDR_LEN + 1;
        } else {
            /* Unknown version — try old format fallback */
            hdr_size = CAMEX_HDR_LEN;
        }
    }

    if (memcmp(packet, CAMEX_MAGIC, 4) != 0) {
        return -1;
    }

    *type = packet[4];
    memcpy(nonce, packet + 5, 12);
    memcpy(&body_len_be, packet + 17, sizeof(body_len_be));
    body_len = (size_t)ntohs(body_len_be);

    if (packet_len != hdr_size + body_len + 16U || body_len > plain_size) {
        return -1;
    }

    crypto_aead_init_ietf(&ctx, key != NULL ? key : crypto_ctx.psk_key, nonce);
    if (crypto_aead_read(&ctx, plain,
                         packet + hdr_size + body_len,
                         packet, hdr_size,
                         packet + hdr_size, body_len) != 0) {
        return -1;
    }

    if (seq_out != NULL) {
        *seq_out = read_be64(packet + 19);
    }

    *plain_len = body_len;
    crypto_wipe(nonce, sizeof(nonce));
    return 0;
}

int replay_check(uint64_t *seq_max, uint64_t *window, uint64_t seq)
{
    uint64_t diff;

    if (seq > *seq_max) {
        diff = seq - *seq_max;
        /* NOTE: diff MUST be < 64 here — shifting a uint64_t by 64 bits
         * (or more) is undefined behaviour in C.  The check on the next
         * line is load-bearing; do not change '<' to '<='. */
        *window = (diff < 64U) ? ((*window << diff) | 1U) : 1U;
        *seq_max = seq;
        return 0;
    }

    diff = *seq_max - seq;
    if (diff >= 64U || ((*window >> diff) & 1U) != 0U) {
        return -1;
    }

    *window |= ((uint64_t)1 << diff);
    return 0;
}
