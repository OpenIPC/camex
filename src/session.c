/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * session.c — protocol v2 wire format and transport sessions (#17)
 *
 */

#include "session.h"
#include "monocypher.h"
#include "util.h"

#include <string.h>

#ifndef _WIN32
#include <sys/time.h>
#else
#include <windows.h>
#endif

uint32_t v2_read_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

void v2_write_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint64_t read_le64(const uint8_t *p)
{
    uint64_t v = 0U;
    int i;

    for (i = 7; i >= 0; --i) {
        v = (v << 8) | p[i];
    }
    return v;
}

static void write_le64(uint8_t *p, uint64_t v)
{
    unsigned int i;

    for (i = 0U; i < 8U; ++i) {
        p[i] = (uint8_t)(v >> (8U * i));
    }
}

uint8_t v2_packet_type(const uint8_t *pkt, size_t len)
{
    if (pkt == NULL || len < 5U || memcmp(pkt, CAMEX_MAGIC, 4) != 0) {
        return 0U;
    }
    switch (pkt[4]) {
    case V2_TYPE_INIT:
        return len >= V2_INIT_HDR + NOISE_MSG1_OVERHEAD + V2_TIMESTAMP_LEN +
                          V2_MAC_LEN
                   ? V2_TYPE_INIT : 0U;
    case V2_TYPE_RESP:
        return len >= V2_RESP_HDR + NOISE_MSG2_OVERHEAD ? V2_TYPE_RESP : 0U;
    case V2_TYPE_DATA:
        return len >= V2_OVERHEAD ? V2_TYPE_DATA : 0U;
    default:
        return 0U;
    }
}

uint32_t v2_data_receiver(const uint8_t *pkt)
{
    return v2_read_u32(pkt + 5);
}

int replay_check(const replay_window_t *r, uint64_t n)
{
    if (n >= V2_REJECT_AFTER_MSGS) {
        return -1;
    }
    if (!r->any || n > r->max) {
        return 0;
    }
    /* The bitmap is a ring of 64-bit words: n must lie in a word that is
     * still in the ring (window of 1985 to 2048 packets) */
    if (r->max / 64U - n / 64U >= V2_REPLAY_WINDOW / 64U) {
        return -1;
    }
    return (r->bits[(n / 64U) % (V2_REPLAY_WINDOW / 64U)] >> (n % 64U)) & 1U
               ? -1 : 0;
}

void replay_record(replay_window_t *r, uint64_t n)
{
    const uint64_t words = V2_REPLAY_WINDOW / 64U;

    if (!r->any) {
        memset(r->bits, 0, sizeof(r->bits));
        r->max = n;
        r->any = 1U;
    } else if (n > r->max) {
        uint64_t cur = r->max / 64U;
        uint64_t top = n / 64U;
        uint64_t w;

        /* Clear the words the window moves over */
        if (top - cur >= words) {
            memset(r->bits, 0, sizeof(r->bits));
        } else {
            for (w = cur + 1U; w <= top; ++w) {
                r->bits[w % words] = 0U;
            }
        }
        r->max = n;
    }
    r->bits[(n / 64U) % words] |= (uint64_t)1 << (n % 64U);
}

void session_init(session_t *s, uint32_t local_index, uint32_t remote_index,
                  const uint8_t k_send[NOISE_KEYLEN],
                  const uint8_t k_recv[NOISE_KEYLEN], time_t now)
{
    memset(s, 0, sizeof(*s));
    s->local_index = local_index;
    s->remote_index = remote_index;
    memcpy(s->k_send, k_send, NOISE_KEYLEN);
    memcpy(s->k_recv, k_recv, NOISE_KEYLEN);
    s->created = now;
    s->valid = 1U;
}

void session_wipe(session_t *s)
{
    crypto_wipe(s, sizeof(*s));
}

size_t session_seal(session_t *s, const uint8_t *plain, size_t len,
                    uint8_t *out, size_t out_size)
{
    if (!s->valid || s->send_n >= V2_REJECT_AFTER_MSGS ||
        out_size < V2_OVERHEAD + len) {
        return 0U;
    }
    memcpy(out, CAMEX_MAGIC, 4);
    out[4] = V2_TYPE_DATA;
    v2_write_u32(out + 5, s->remote_index);
    write_le64(out + 9, s->send_n);
    noise_encrypt(s->k_send, s->send_n, out, V2_DATA_HDR, plain, len,
                  out + V2_DATA_HDR);
    s->send_n++;
    return V2_OVERHEAD + len;
}

int session_open(session_t *s, const uint8_t *pkt, size_t len,
                 uint8_t *plain, size_t plain_size, size_t *plain_len)
{
    uint64_t n;

    if (!s->valid || v2_packet_type(pkt, len) != V2_TYPE_DATA ||
        v2_data_receiver(pkt) != s->local_index ||
        plain_size < len - V2_OVERHEAD) {
        return -1;
    }
    n = read_le64(pkt + 9);
    /* Cheap checks first; the counter is recorded only once the packet
     * has been authenticated */
    if (replay_check(&s->replay, n) != 0) {
        return -1;
    }
    if (noise_decrypt(s->k_recv, n, pkt, V2_DATA_HDR, pkt + V2_DATA_HDR,
                      len - V2_DATA_HDR, plain) != 0) {
        return -1;
    }
    replay_record(&s->replay, n);
    *plain_len = len - V2_OVERHEAD;
    return 0;
}

void v2_timestamp(uint8_t out[V2_TIMESTAMP_LEN])
{
    uint64_t sec;
    uint32_t nsec;

#ifndef _WIN32
    struct timeval tv;

    gettimeofday(&tv, NULL);
    sec = (uint64_t)tv.tv_sec;
    nsec = (uint32_t)tv.tv_usec * 1000U;
#else
    FILETIME ft;
    uint64_t t;

    GetSystemTimeAsFileTime(&ft);
    t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    t -= 116444736000000000ULL;          /* 1601 -> 1970, 100 ns units */
    sec = t / 10000000ULL;
    nsec = (uint32_t)(t % 10000000ULL) * 100U;
#endif
    sec += 0x400000000000000aULL;        /* TAI64 label (with 10 s offset) */
    v2_write_u32(out, (uint32_t)(sec >> 32));
    v2_write_u32(out + 4, (uint32_t)sec);
    v2_write_u32(out + 8, nsec);
}

uint64_t v2_timestamp_seconds(const uint8_t ts[V2_TIMESTAMP_LEN])
{
    uint64_t sec = ((uint64_t)v2_read_u32(ts) << 32) | v2_read_u32(ts + 4);

    return sec >= 0x400000000000000aULL ? sec - 0x400000000000000aULL : 0U;
}

int v2_timestamp_cmp(const uint8_t a[V2_TIMESTAMP_LEN],
                     const uint8_t b[V2_TIMESTAMP_LEN])
{
    return memcmp(a, b, V2_TIMESTAMP_LEN);
}

uint32_t v2_new_index(void)
{
    uint8_t b[4];
    uint32_t v = 0U;
    int tries;

    for (tries = 0; v == 0U && tries < 8; ++tries) {
        if (get_random_bytes(b, sizeof(b)) != 0) {
            return 0U;
        }
        v = v2_read_u32(b);
    }
    return v;
}

void v2_mac1(const uint8_t server_pub[32], const uint8_t *pkt, size_t len,
             uint8_t out[V2_MAC_LEN])
{
    crypto_blake2b_ctx ctx;
    uint8_t key[32];

    crypto_blake2b_init(&ctx, sizeof(key));
    crypto_blake2b_update(&ctx, (const uint8_t *)"mac1----", 8);
    crypto_blake2b_update(&ctx, server_pub, 32);
    crypto_blake2b_final(&ctx, key);
    crypto_blake2b_keyed(out, V2_MAC_LEN, key, sizeof(key), pkt, len);
    crypto_wipe(key, sizeof(key));
}
