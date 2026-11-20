/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * chacha_neon.h — ChaCha20 with NEON on 32-bit ARM (chacha_neon.c), used
 *                 by monocypher's crypto_chacha20_djb() when built with
 *                 CAMEX_CHACHA_NEON and the CPU has NEON
 *
 */

#ifndef CAMEX_CHACHA_NEON_H
#define CAMEX_CHACHA_NEON_H

#include <stdint.h>
#include <stddef.h>

/* Whole groups of four 64-byte blocks of the ChaCha20 state input (its
 * counter in words 12 and 13 advanced): out = in ^ keystream, or the
 * keystream with in NULL. Returns the blocks done (a multiple of 4, fewer
 * than asked when the 32-bit counter would wrap inside a group). */
size_t camex_chacha20_neon(uint8_t *out, const uint8_t *in, size_t blocks,
                           uint32_t input[16]);

/* 1 when the CPU has NEON (the kernel's AT_HWCAP) */
int camex_chacha_neon_ok(void);

#endif /* CAMEX_CHACHA_NEON_H */
