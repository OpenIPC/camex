/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * test_keys.c — key material for protocol v2 (#17, #14)
 *
 */

#include "keys.h"
#include "monocypher.h"
#include "noise.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static int failures = 0;

static void check(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

int main(void)
{
    uint8_t key[KEY_LEN];
    uint8_t key2[KEY_LEN];
    uint8_t raw[KEY_LEN];
    uint8_t buf[64];
    char text[KEY_B64_LEN + 1U];
    char priv[KEY_B64_LEN + 1U];
    char pub[KEY_B64_LEN + 1U];
    uint8_t spriv[KEY_LEN], spub[KEY_LEN], npub[KEY_LEN], npriv[KEY_LEN];
    size_t i;
    clock_t t0;
    double first, second;

    /* base64 */
    check(key_base64_encode((const uint8_t *)"foob", 4U, text, sizeof(text))
          == 8U && strcmp(text, "Zm9vYg==") == 0, "base64 encode");
    check(key_base64_decode("Zm9vYmFy", buf, sizeof(buf)) == 6 &&
          memcmp(buf, "foobar", 6) == 0, "base64 decode");
    check(key_base64_decode("Zm9vYg=", buf, sizeof(buf)) < 0 &&
          key_base64_decode("Zm9=Yg==", buf, sizeof(buf)) < 0 &&
          key_base64_decode("Zm9v!g==", buf, sizeof(buf)) < 0,
          "malformed base64 is rejected");

    /* raw keys */
    for (i = 0U; i < KEY_LEN; ++i) {
        raw[i] = (uint8_t)i;
    }
    check(key_from_psk("hex:000102030405060708090a0b0c0d0e0f"
                       "101112131415161718191A1B1C1D1E1F", key) == 0 &&
          memcmp(key, raw, KEY_LEN) == 0, "hex: key");
    key_base64_encode(raw, KEY_LEN, text, sizeof(text));
    {
        char b64key[8 + KEY_B64_LEN + 1U];
        snprintf(b64key, sizeof(b64key), "base64:%s", text);
        check(key_from_psk(b64key, key) == 0 && memcmp(key, raw, KEY_LEN) == 0,
              "base64: key");
    }
    check(key_from_psk("hex:0001", key) != 0 &&
          key_from_psk("hex:zz0102030405060708090a0b0c0d0e0f"
                       "101112131415161718191a1b1c1d1e1f", key) != 0 &&
          key_from_psk("base64:AAAA", key) != 0 && key_from_psk("", key) != 0,
          "malformed raw keys are rejected");
    check(key_from_psk("hex:0000000000000000000000000000000000000000000000"
                       "000000000000000000", key) != 0 &&
          key_from_psk("base64:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=",
                       key) != 0,
          "all-zero raw keys are rejected");

    /* Argon2id: RFC 9106 test vector (Monocypher build check) */
    {
        static const uint8_t want[32] = {
            0x0d, 0x64, 0x0d, 0xf5, 0x8d, 0x78, 0x76, 0x6c, 0x08, 0xc0, 0x37,
            0xa3, 0x4a, 0x8b, 0x53, 0xc9, 0xd0, 0x1e, 0xf0, 0x45, 0x2d, 0x75,
            0xb6, 0x5e, 0xb5, 0x25, 0x20, 0xe9, 0x6b, 0x01, 0xe6, 0x59 };
        uint8_t pass[32], salt[16], secret[8], ad[12], out[32];
        crypto_argon2_config c;
        crypto_argon2_inputs in;
        crypto_argon2_extras ex;
        void *work = malloc(32U * 1024U);

        memset(pass, 1, sizeof(pass));
        memset(salt, 2, sizeof(salt));
        memset(secret, 3, sizeof(secret));
        memset(ad, 4, sizeof(ad));
        c.algorithm = CRYPTO_ARGON2_ID;
        c.nb_blocks = 32U;
        c.nb_passes = 3U;
        c.nb_lanes = 4U;
        in.pass = pass;
        in.pass_size = sizeof(pass);
        in.salt = salt;
        in.salt_size = sizeof(salt);
        ex.key = secret;
        ex.key_size = sizeof(secret);
        ex.ad = ad;
        ex.ad_size = sizeof(ad);
        crypto_argon2(out, sizeof(out), work, c, in, ex);
        free(work);
        check(memcmp(out, want, sizeof(want)) == 0, "Argon2id RFC 9106 vector");
    }

    /* Passphrase: deterministic, cached, different passphrases differ */
    t0 = clock();
    check(key_from_psk("correct horse battery staple", key) == 0,
          "passphrase key");
    first = (double)(clock() - t0) / CLOCKS_PER_SEC;
    t0 = clock();
    check(key_from_psk("correct horse battery staple", key2) == 0 &&
          memcmp(key, key2, KEY_LEN) == 0, "passphrase key is deterministic");
    second = (double)(clock() - t0) / CLOCKS_PER_SEC;
    check(second < first, "second derivation comes from the cache");
    key_cache_clear();
    check(key_from_psk("correct horse battery staple", key2) == 0 &&
          memcmp(key, key2, KEY_LEN) == 0, "same key after the cache is cleared");
    check(key_from_psk("correct horse battery stapler", key2) == 0 &&
          memcmp(key, key2, KEY_LEN) != 0, "another passphrase, another key");
    printf("argon2id: %.3f s, cached: %.6f s\n", first, second);

    /* Server key pair */
    check(key_generate(priv, pub) == 0 && strlen(priv) == KEY_B64_LEN &&
          strlen(pub) == KEY_B64_LEN, "key pair generated");
    check(key_server_from_b64(priv, spriv, spub) == 0 &&
          key_parse_b64(pub, npub) == 0 && memcmp(spub, npub, KEY_LEN) == 0,
          "public key matches the private key");
    noise_keypair(npriv, npub, spriv);
    check(memcmp(npub, spub, KEY_LEN) == 0, "same public key as Noise");
    check(key_server_from_b64("tooshort", spriv, spub) != 0,
          "malformed private key is rejected");

    if (failures != 0) {
        fprintf(stderr, "test_keys: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_keys: OK\n");
    return 0;
}
