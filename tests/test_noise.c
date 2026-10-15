/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * test_noise.c — Noise_NKpsk0_25519_ChaChaPoly_BLAKE2b against the
 * cacophony test vector; HMAC-BLAKE2b against a reference value (#17)
 *
 */

#include "noise.h"

#include <stdio.h>
#include <string.h>

#include "noise_vectors.h"

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
    noise_hs_t ini;
    noise_hs_t resp;
    noise_hs_t try_hs;
    uint8_t s_priv[NOISE_DHLEN];
    uint8_t s_pub[NOISE_DHLEN];
    uint8_t msg[256];
    uint8_t plain[256];
    uint8_t es[NOISE_DHLEN];
    uint8_t wrong_psk[NOISE_KEYLEN];
    uint8_t i_c1[NOISE_KEYLEN], i_c2[NOISE_KEYLEN];
    uint8_t r_c1[NOISE_KEYLEN], r_c2[NOISE_KEYLEN];
    size_t len;
    size_t plen = 0U;
    unsigned int m;
    uint64_t n_ini = 0U;
    uint64_t n_resp = 0U;

    noise_keypair(s_priv, s_pub, v_resp_s);
    noise_init_initiator(&ini, v_prologue, sizeof(v_prologue), s_pub);
    noise_init_responder(&resp, v_prologue, sizeof(v_prologue),
                         s_priv, s_pub);

    /* Message 1: -> psk, e, es */
    len = noise_write_msg1(&ini, v_psk, v_init_e, v_p[0], v_pl[0],
                           msg, sizeof(msg));
    check(len == v_cl[0] && memcmp(msg, v_c[0], len) == 0,
          "message 1 matches the vector");
    check(noise_msg1_es(&resp, msg, len, es) == 0, "es computed");

    /* A wrong PSK is rejected and leaves the template untouched */
    memset(wrong_psk, 0x11, sizeof(wrong_psk));
    try_hs = resp;
    check(noise_read_msg1(&try_hs, wrong_psk, es, msg, len, plain,
                          sizeof(plain), &plen) != 0,
          "message 1 with a wrong PSK is rejected");
    try_hs = resp;
    check(noise_read_msg1(&try_hs, v_psk, es, msg, len, plain,
                          sizeof(plain), &plen) == 0 &&
          plen == v_pl[0] && memcmp(plain, v_p[0], plen) == 0,
          "message 1 decrypts with the right PSK");
    resp = try_hs;

    /* A tampered message 1 is rejected */
    msg[len - 1] ^= 0x01U;
    try_hs = resp;
    noise_init_responder(&try_hs, v_prologue, sizeof(v_prologue),
                         s_priv, s_pub);
    check(noise_read_msg1(&try_hs, v_psk, es, msg, len, plain,
                          sizeof(plain), &plen) != 0,
          "tampered message 1 is rejected");

    /* Message 2: <- e, ee */
    len = noise_write_msg2(&resp, v_resp_e, v_p[1], v_pl[1],
                           msg, sizeof(msg));
    check(len == v_cl[1] && memcmp(msg, v_c[1], len) == 0,
          "message 2 matches the vector");
    check(noise_read_msg2(&ini, msg, len, plain, sizeof(plain), &plen) == 0 &&
          plen == v_pl[1] && memcmp(plain, v_p[1], plen) == 0,
          "message 2 decrypts");
    check(memcmp(ini.h, v_hash, NOISE_HASHLEN) == 0 &&
          memcmp(resp.h, v_hash, NOISE_HASHLEN) == 0,
          "handshake hash matches the vector");

    noise_split(&ini, i_c1, i_c2);
    noise_split(&resp, r_c1, r_c2);
    check(memcmp(i_c1, r_c1, NOISE_KEYLEN) == 0 &&
          memcmp(i_c2, r_c2, NOISE_KEYLEN) == 0 &&
          memcmp(i_c1, i_c2, NOISE_KEYLEN) != 0,
          "split keys agree and differ per direction");

    /* Transport messages alternate, starting with the initiator */
    for (m = 2U; m < V_MSGS; ++m) {
        int from_ini = (m % 2U) == 0U;
        const uint8_t *k = from_ini ? i_c1 : r_c2;
        uint64_t n = from_ini ? n_ini++ : n_resp++;
        char what[64];

        noise_encrypt(k, n, NULL, 0U, v_p[m], v_pl[m], msg);
        snprintf(what, sizeof(what), "transport message %u matches", m);
        check(v_pl[m] + NOISE_TAGLEN == v_cl[m] &&
              memcmp(msg, v_c[m], v_cl[m]) == 0, what);
        snprintf(what, sizeof(what), "transport message %u decrypts", m);
        check(noise_decrypt(from_ini ? r_c1 : i_c2, n, NULL, 0U, v_c[m],
                            v_cl[m], plain) == 0 &&
              memcmp(plain, v_p[m], v_pl[m]) == 0, what);
    }
    msg[0] ^= 0x01U;
    check(noise_decrypt(r_c1, 0U, NULL, 0U, msg, v_cl[2], plain) != 0,
          "tampered transport message is rejected");

    noise_wipe(&ini);
    noise_wipe(&resp);
    if (failures != 0) {
        fprintf(stderr, "test_noise: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_noise: OK\n");
    return 0;
}
