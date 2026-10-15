/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * test_packet.c — unit tests for the packet backend interface (#20),
 * with an in-memory backend
 *
 */

#include "camex.h"
#include "client.h"
#include "packet.h"

#include <stdio.h>
#include <string.h>

/* Globals normally provided by camex.c / client.c */
camex_config_t current_config;
volatile sig_atomic_t running = 1;
volatile sig_atomic_t reload_config = 0;
time_t g_now = 0;
uint8_t client_mode = 0;
uint8_t server_mode = 0;
client_state_t client_state;

/* In-memory backend: one queued packet each way */
static uint8_t mem_in[64];
static size_t mem_in_len = 0U;
static uint8_t mem_out[64];
static size_t mem_out_len = 0U;
static int mem_opened = 0;
static int mem_mtu = 0;

static int mem_open(const camex_packet_open_t *p)
{
    mem_opened = 1;
    mem_mtu = p->mtu;
    return 0;
}

static ssize_t mem_read(uint8_t *buffer, size_t size)
{
    size_t n = mem_in_len < size ? mem_in_len : size;

    memcpy(buffer, mem_in, n);
    mem_in_len = 0U;
    return (ssize_t)n;
}

static int mem_write(const uint8_t *buffer, size_t len)
{
    if (len > sizeof(mem_out)) {
        return -1;
    }
    memcpy(mem_out, buffer, len);
    mem_out_len = len;
    return 0;
}

static intptr_t mem_fd(void)
{
    return 42;
}

static void mem_close(void)
{
    mem_opened = 0;
}

static const camex_packet_backend_t mem_backend = {
    "memory", mem_open, mem_read, mem_write, mem_fd, mem_close, NULL
};

int main(void)
{
    camex_packet_open_t params;
    uint8_t buf[64];
    int failures = 0;

    if (packet_backend_get() != &camex_tun_backend) {
        fprintf(stderr, "FAIL: TUN is not the default backend\n");
        ++failures;
    }

    /* close without open does not reach the backend */
    mem_opened = 7;
    packet_backend_set(&mem_backend);
    packet_close();
    if (mem_opened != 7) {
        fprintf(stderr, "FAIL: close reached a backend that was not open\n");
        ++failures;
    }
    mem_opened = 0;
    packet_backend_set(NULL);

    if (packet_backend_set(&mem_backend) != 0) {
        fprintf(stderr, "FAIL: could not select a backend\n");
        ++failures;
    }
    memset(&params, 0, sizeof(params));
    params.mtu = 1400;
    if (packet_open(&params) != 0 || !mem_opened || mem_mtu != 1400) {
        fprintf(stderr, "FAIL: open did not reach the backend\n");
        ++failures;
    }
    if (packet_write((const uint8_t *)"\x45\x00", 2U) != 0 ||
        mem_out_len != 2U || mem_out[0] != 0x45) {
        fprintf(stderr, "FAIL: write did not reach the backend\n");
        ++failures;
    }
    mem_in[0] = 0x45;
    mem_in_len = 20U;
    if (packet_read(buf, sizeof(buf)) != 20 || buf[0] != 0x45) {
        fprintf(stderr, "FAIL: read did not come from the backend\n");
        ++failures;
    }
    if (packet_backend_set(NULL) == 0 ||
        packet_backend_get() != &mem_backend) {
        fprintf(stderr, "FAIL: the backend changed while open\n");
        ++failures;
    }
    if (packet_host_routes()) {
        fprintf(stderr, "FAIL: a backend without host_routes got routes\n");
        ++failures;
    }
    if (packet_fd() != 42) {
        fprintf(stderr, "FAIL: fd did not come from the backend\n");
        ++failures;
    }
    packet_close();
    if (mem_opened) {
        fprintf(stderr, "FAIL: close did not reach the backend\n");
        ++failures;
    }

    if (packet_backend_set(NULL) != 0 ||
        packet_backend_get() != &camex_tun_backend) {
        fprintf(stderr, "FAIL: NULL does not restore the TUN backend\n");
        ++failures;
    }

    if (failures != 0) {
        fprintf(stderr, "test_packet: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_packet: OK\n");
    return 0;
}
