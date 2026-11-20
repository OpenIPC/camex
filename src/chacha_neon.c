/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * chacha_neon.c — ChaCha20, four blocks at a time with NEON, for 32-bit
 *                 ARM (Cortex-A7 and the like); monocypher's
 *                 crypto_chacha20_djb() calls it for whole groups of four
 *                 blocks when the CPU has NEON, and does the rest itself.
 *
 * This file is built with -mfpu=neon for armv7-a, and run only when the
 * kernel says the CPU has NEON (AT_HWCAP), so one binary still runs on
 * ARMv5 and ARMv6 cameras.
 *
 */

#include "chacha_neon.h"

#include <stdio.h>
#include <string.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>

#define ROTL(v, n) vsriq_n_u32(vshlq_n_u32((v), (n)), (v), 32 - (n))
#define ROTL16(v) \
    vreinterpretq_u32_u16(vrev32q_u16(vreinterpretq_u16_u32(v)))

#define QR(a, b, c, d) do { \
        a = vaddq_u32(a, b); d = veorq_u32(d, a); d = ROTL16(d); \
        c = vaddq_u32(c, d); b = veorq_u32(b, c); b = ROTL(b, 12); \
        a = vaddq_u32(a, b); d = veorq_u32(d, a); d = ROTL(d, 8); \
        c = vaddq_u32(c, d); b = veorq_u32(b, c); b = ROTL(b, 7); \
    } while (0)

/* Rows j of the four words a..d: block j's 16 bytes */
static void put4(uint8_t *out, const uint8_t *in, uint32x4_t a, uint32x4_t b,
                 uint32x4_t c, uint32x4_t d)
{
    uint32x4x2_t t0 = vtrnq_u32(a, b);
    uint32x4x2_t t1 = vtrnq_u32(c, d);
    uint32x4_t r[4];
    int j;

    r[0] = vcombine_u32(vget_low_u32(t0.val[0]), vget_low_u32(t1.val[0]));
    r[1] = vcombine_u32(vget_low_u32(t0.val[1]), vget_low_u32(t1.val[1]));
    r[2] = vcombine_u32(vget_high_u32(t0.val[0]), vget_high_u32(t1.val[0]));
    r[3] = vcombine_u32(vget_high_u32(t0.val[1]), vget_high_u32(t1.val[1]));
    for (j = 0; j < 4; ++j) {
        uint8x16_t k = vreinterpretq_u8_u32(r[j]);

        if (in != NULL) {
            k = veorq_u8(k, vld1q_u8(in + j * 64));
        }
        vst1q_u8(out + j * 64, k);
    }
}

size_t camex_chacha20_neon(uint8_t *out, const uint8_t *in, size_t blocks,
                           uint32_t input[16])
{
    size_t done = 0U;

    while (blocks - done >= 4U) {
        uint32x4_t x[16];
        uint32x4_t o[16];
        const uint32_t c = input[12];
        int i;

        /* The four counters must not wrap inside the group: the caller
         * does such a group itself */
        if (c > 0xFFFFFFFFU - 3U) {
            break;
        }
        for (i = 0; i < 16; ++i) {
            o[i] = vdupq_n_u32(input[i]);
        }
        {
            const uint32_t ctr[4] = { c, c + 1U, c + 2U, c + 3U };

            o[12] = vld1q_u32(ctr);
        }
        for (i = 0; i < 16; ++i) {
            x[i] = o[i];
        }
        for (i = 0; i < 10; ++i) {
            QR(x[0], x[4], x[8], x[12]);
            QR(x[1], x[5], x[9], x[13]);
            QR(x[2], x[6], x[10], x[14]);
            QR(x[3], x[7], x[11], x[15]);
            QR(x[0], x[5], x[10], x[15]);
            QR(x[1], x[6], x[11], x[12]);
            QR(x[2], x[7], x[8], x[13]);
            QR(x[3], x[4], x[9], x[14]);
        }
        for (i = 0; i < 16; ++i) {
            x[i] = vaddq_u32(x[i], o[i]);
        }
        put4(out, in, x[0], x[1], x[2], x[3]);
        put4(out + 16, in != NULL ? in + 16 : NULL, x[4], x[5], x[6], x[7]);
        put4(out + 32, in != NULL ? in + 32 : NULL, x[8], x[9], x[10], x[11]);
        put4(out + 48, in != NULL ? in + 48 : NULL, x[12], x[13], x[14],
             x[15]);
        out += 256;
        if (in != NULL) {
            in += 256;
        }
        input[12] = c + 4U;
        if (input[12] == 0U) {
            input[13]++;
        }
        done += 4U;
    }
    return done;
}

/* AT_HWCAP & HWCAP_NEON from the kernel's auxiliary vector: no libc
 * function needed (old uClibc has no getauxval) */
#define AUX_AT_HWCAP 16UL
#define AUX_HWCAP_NEON (1UL << 12)

int camex_chacha_neon_ok(void)
{
    static int state = -1;

    if (state < 0) {
        FILE *fp = fopen("/proc/self/auxv", "rb");
        unsigned long pair[2];

        state = 0;
        if (fp != NULL) {
            while (fread(pair, sizeof(pair), 1U, fp) == 1U && pair[0] != 0UL) {
                if (pair[0] == AUX_AT_HWCAP) {
                    state = (pair[1] & AUX_HWCAP_NEON) != 0UL;
                    break;
                }
            }
            fclose(fp);
        }
    }
    return state;
}

#else   /* built without NEON: never used */

size_t camex_chacha20_neon(uint8_t *out, const uint8_t *in, size_t blocks,
                           uint32_t input[16])
{
    (void)out;
    (void)in;
    (void)blocks;
    (void)input;
    return 0U;
}

int camex_chacha_neon_ok(void)
{
    return 0;
}
#endif
