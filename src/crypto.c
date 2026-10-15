/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * crypto.c — key context: global PSK, server key pair (protocol v2)
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
    crypto_ctx.ready = 1U;
    if (psk == NULL || *psk == '\0') {
        crypto_ctx.has_global = 0U;   /* per-client keys only */
        return 0;
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
    crypto_ctx.has_global = 1U;
    crypto_log_fingerprint();
    return 0;
}

void crypto_save_ctx(camex_crypto_t *out)
{
    *out = crypto_ctx;
}

void crypto_restore_ctx(const camex_crypto_t *in)
{
    crypto_ctx = *in;
}

void crypto_drop_global(void)
{
    crypto_wipe(crypto_ctx.psk_key, sizeof(crypto_ctx.psk_key));
    crypto_ctx.has_global = 0U;
}

void crypto_wipe_ctx(void)
{
    crypto_wipe(&crypto_ctx, sizeof(crypto_ctx));
    crypto_ctx.ready = 0U;
}
