/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * chacha_neon.c — ChaCha20 (four blocks at a time) and Poly1305 (two
 *                 blocks at a time) with NEON, for 32-bit ARM (Cortex-A7
 *                 and the like); monocypher's crypto_chacha20_djb() and
 *                 crypto_poly1305_update() call them for whole groups of
 *                 blocks when the CPU has NEON, and do the rest themselves.
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

/* Poly1305, two blocks at a time: 26-bit limbs, the even blocks in lane 0
 * and the odd ones in lane 1, each lane multiplied by r^2 per step and the
 * two put together at the end as lane0 * r^2 + lane1 * r (the usual NEON
 * method). Constant time, like the plain code. */
#define M26 0x3ffffffU

static void p_to26(uint32_t t[5], const uint32_t w[4], uint32_t top)
{
    t[0] = w[0] & M26;
    t[1] = ((w[0] >> 26) | (w[1] << 6)) & M26;
    t[2] = ((w[1] >> 20) | (w[2] << 12)) & M26;
    t[3] = ((w[2] >> 14) | (w[3] << 18)) & M26;
    t[4] = (w[3] >> 8) | (top << 24);
}

/* All limbs below 2^26 (the carry out of the top folded in times 5) */
static void p_carry(uint64_t d[5], uint32_t t[5])
{
    uint64_t c;

    c = d[0] >> 26; t[0] = (uint32_t)d[0] & M26; d[1] += c;
    c = d[1] >> 26; t[1] = (uint32_t)d[1] & M26; d[2] += c;
    c = d[2] >> 26; t[2] = (uint32_t)d[2] & M26; d[3] += c;
    c = d[3] >> 26; t[3] = (uint32_t)d[3] & M26; d[4] += c;
    c = d[4] >> 26; t[4] = (uint32_t)d[4] & M26;
    c = (uint64_t)t[0] + c * 5U;
    t[0] = (uint32_t)c & M26;
    c = (uint64_t)t[1] + (c >> 26);
    t[1] = (uint32_t)c & M26;
    t[2] += (uint32_t)(c >> 26);
}

/* t = a * b mod 2^130 - 5, limbs below 2^26 */
static void p_mul(uint32_t t[5], const uint32_t a[5], const uint32_t b[5])
{
    const uint64_t s1 = b[1] * 5U, s2 = b[2] * 5U, s3 = b[3] * 5U,
                   s4 = b[4] * 5U;
    uint64_t d[5];

    d[0] = (uint64_t)a[0] * b[0] + a[1] * s4 + a[2] * s3 + a[3] * s2 +
           a[4] * s1;
    d[1] = (uint64_t)a[0] * b[1] + (uint64_t)a[1] * b[0] + a[2] * s4 +
           a[3] * s3 + a[4] * s2;
    d[2] = (uint64_t)a[0] * b[2] + (uint64_t)a[1] * b[1] +
           (uint64_t)a[2] * b[0] + a[3] * s4 + a[4] * s3;
    d[3] = (uint64_t)a[0] * b[3] + (uint64_t)a[1] * b[2] +
           (uint64_t)a[2] * b[1] + (uint64_t)a[3] * b[0] + a[4] * s4;
    d[4] = (uint64_t)a[0] * b[4] + (uint64_t)a[1] * b[3] +
           (uint64_t)a[2] * b[2] + (uint64_t)a[3] * b[1] +
           (uint64_t)a[4] * b[0];
    p_carry(d, t);
}

/* Both lanes times (r lane 0, r lane 1), then carried: limbs < 2^26 + a
 * little (limb 1 may carry a few bits more, as in poly1305-donna) */
static void p_mul2(uint32x2_t h[5], const uint32x2_t r[5],
                   const uint32x2_t s[5])
{
    const uint32x2_t m = vdup_n_u32(M26);
    uint64x2_t d0, d1, d2, d3, d4;
    uint64x2_t c;

    d0 = vmull_u32(h[0], r[0]);
    d0 = vmlal_u32(d0, h[1], s[4]);
    d0 = vmlal_u32(d0, h[2], s[3]);
    d0 = vmlal_u32(d0, h[3], s[2]);
    d0 = vmlal_u32(d0, h[4], s[1]);
    d1 = vmull_u32(h[0], r[1]);
    d1 = vmlal_u32(d1, h[1], r[0]);
    d1 = vmlal_u32(d1, h[2], s[4]);
    d1 = vmlal_u32(d1, h[3], s[3]);
    d1 = vmlal_u32(d1, h[4], s[2]);
    d2 = vmull_u32(h[0], r[2]);
    d2 = vmlal_u32(d2, h[1], r[1]);
    d2 = vmlal_u32(d2, h[2], r[0]);
    d2 = vmlal_u32(d2, h[3], s[4]);
    d2 = vmlal_u32(d2, h[4], s[3]);
    d3 = vmull_u32(h[0], r[3]);
    d3 = vmlal_u32(d3, h[1], r[2]);
    d3 = vmlal_u32(d3, h[2], r[1]);
    d3 = vmlal_u32(d3, h[3], r[0]);
    d3 = vmlal_u32(d3, h[4], s[4]);
    d4 = vmull_u32(h[0], r[4]);
    d4 = vmlal_u32(d4, h[1], r[3]);
    d4 = vmlal_u32(d4, h[2], r[2]);
    d4 = vmlal_u32(d4, h[3], r[1]);
    d4 = vmlal_u32(d4, h[4], r[0]);

    c = vshrq_n_u64(d0, 26); h[0] = vand_u32(vmovn_u64(d0), m);
    d1 = vaddq_u64(d1, c);
    c = vshrq_n_u64(d1, 26); h[1] = vand_u32(vmovn_u64(d1), m);
    d2 = vaddq_u64(d2, c);
    c = vshrq_n_u64(d2, 26); h[2] = vand_u32(vmovn_u64(d2), m);
    d3 = vaddq_u64(d3, c);
    c = vshrq_n_u64(d3, 26); h[3] = vand_u32(vmovn_u64(d3), m);
    d4 = vaddq_u64(d4, c);
    c = vshrq_n_u64(d4, 26); h[4] = vand_u32(vmovn_u64(d4), m);
    /* h0 + c * 5: h limbs below 2^27.1 and r below 2^26 keep d4 below
     * 2^55.5, so c fits 32 bits */
    d0 = vmlal_u32(vmovl_u32(h[0]), vmovn_u64(c), vdup_n_u32(5U));
    h[0] = vand_u32(vmovn_u64(d0), m);
    h[1] = vadd_u32(h[1], vmovn_u64(vshrq_n_u64(d0, 26)));
}

size_t camex_poly1305_neon(uint32_t h[5], const uint32_t r[4],
                           const uint8_t *in, size_t blocks)
{
    uint32_t r1[5], r2[5], t[5], u[5];
    uint32x2_t H[5], R[5], S[5], Rf[5], Sf[5];
    const uint32x2_t m = vdup_n_u32(M26);
    const uint32x2_t hibit = vdup_n_u32(1U << 24);
    size_t done;
    int i;

    blocks &= ~(size_t)1U;
    if (blocks < 2U) {
        return 0U;
    }
    p_to26(r1, r, 0U);
    p_mul(r2, r1, r1);
    for (i = 0; i < 5; ++i) {
        R[i] = vdup_n_u32(r2[i]);
        S[i] = vdup_n_u32(r2[i] * 5U);
        /* The last step: lane 0 times r^2, lane 1 times r */
        Rf[i] = vset_lane_u32(r1[i], vdup_n_u32(r2[i]), 1);
        Sf[i] = vset_lane_u32(r1[i] * 5U, vdup_n_u32(r2[i] * 5U), 1);
    }
    /* h (radix 2^32, h[4] <= 4) into lane 0 */
    p_to26(t, h, h[4]);
    for (i = 0; i < 5; ++i) {
        H[i] = vset_lane_u32(t[i], vdup_n_u32(0U), 0);
    }
    for (done = 0U; done < blocks; done += 2U) {
        /* Lane 0: words of block done, lane 1: of block done + 1 */
        uint32x2x4_t w = vld4_u32((const uint32_t *)(const void *)in);

        if (done != 0U) {
            p_mul2(H, R, S);
        }
        H[0] = vadd_u32(H[0], vand_u32(w.val[0], m));
        H[1] = vadd_u32(H[1], vand_u32(vorr_u32(vshr_n_u32(w.val[0], 26),
                                                vshl_n_u32(w.val[1], 6)), m));
        H[2] = vadd_u32(H[2], vand_u32(vorr_u32(vshr_n_u32(w.val[1], 20),
                                                vshl_n_u32(w.val[2], 12)), m));
        H[3] = vadd_u32(H[3], vand_u32(vorr_u32(vshr_n_u32(w.val[2], 14),
                                                vshl_n_u32(w.val[3], 18)), m));
        H[4] = vadd_u32(H[4], vorr_u32(vshr_n_u32(w.val[3], 8), hibit));
        in += 32;
    }
    p_mul2(H, Rf, Sf);
    {
        uint64_t d[5];
        uint64_t acc;

        for (i = 0; i < 5; ++i) {
            d[i] = (uint64_t)vget_lane_u32(H[i], 0) + vget_lane_u32(H[i], 1);
        }
        p_carry(d, u);
        /* Back to radix 2^32 by adding (a limb may be 2^26 or a little
         * more): the value is below 2^130 + 2^80, so h[4] <= 4 */
        acc = u[0] + ((uint64_t)u[1] << 26);
        h[0] = (uint32_t)acc;
        acc = (acc >> 32) + ((uint64_t)u[2] << 20);
        h[1] = (uint32_t)acc;
        acc = (acc >> 32) + ((uint64_t)u[3] << 14);
        h[2] = (uint32_t)acc;
        acc = (acc >> 32) + ((uint64_t)u[4] << 8);
        h[3] = (uint32_t)acc;
        h[4] = (uint32_t)(acc >> 32);
    }
    return blocks;
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

size_t camex_poly1305_neon(uint32_t h[5], const uint32_t r[4],
                           const uint8_t *in, size_t blocks)
{
    (void)h;
    (void)r;
    (void)in;
    (void)blocks;
    return 0U;
}

int camex_chacha_neon_ok(void)
{
    return 0;
}
#endif
