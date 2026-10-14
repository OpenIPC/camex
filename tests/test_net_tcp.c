/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * test_net_tcp.c — unit tests for TCP framing and pending-send handling
 *
 */

#include "camex.h"
#include "net.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Globals normally provided by camex.c */
camex_config_t current_config;
volatile sig_atomic_t running = 1;
volatile sig_atomic_t reload_config = 0;
time_t g_now = 0;
uint8_t client_mode = 0;
uint8_t server_mode = 0;

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
        ++failures; \
    } \
} while (0)

/* Nonblocking stream socket pair with small buffers so that sends block early. */
static int make_pair(int sv[2])
{
    int small = 4096;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        perror("socketpair");
        return -1;
    }
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
    setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
    set_fd_nonblocking(sv[0]);
    set_fd_nonblocking(sv[1]);
    return 0;
}

/* Send frames until one is left pending. Returns 0 when pending, -1 otherwise. */
static int fill_until_pending(int fd)
{
    uint8_t payload[1400];
    int i;

    memset(payload, 0xAA, sizeof(payload));
    for (i = 0; i < 10000; ++i) {
        int rc = net_tcp_send_frame(fd, payload, sizeof(payload));
        if (rc == 1) {
            return net_tcp_get_pending_fd() >= 0 ? 0 : -1;
        }
        if (rc != 0) {
            return -1;
        }
    }
    return -1;
}

/* #8: closing a socket that has unsent data must drop that data. */
static void test_close_clears_pending(void)
{
    int a[2];
    int b[2];
    uint8_t frame[3] = { 1, 2, 3 };
    uint8_t got[5];
    ssize_t n;
    int stale_fd;

    if (make_pair(a) != 0) {
        ++failures;
        return;
    }

    CHECK(fill_until_pending(a[0]) == 0, "could not get a pending send");
    stale_fd = a[0];

    net_tcp_close(a[0]);
    close(a[1]);

    CHECK(net_tcp_get_pending_fd() < 0,
          "pending fd still set after close (select() would get EBADF)");

    /* A new connection that reuses the same fd number must not receive
     * the tail of the old frame. */
    if (make_pair(b) != 0) {
        ++failures;
        return;
    }
    if (b[0] != stale_fd) {
        if (dup2(b[0], stale_fd) < 0) {
            perror("dup2");
            ++failures;
            return;
        }
        close(b[0]);
        b[0] = stale_fd;
        set_fd_nonblocking(b[0]);
    }

    CHECK(net_tcp_send_frame(b[0], frame, sizeof(frame)) == 0,
          "send on reused fd did not complete");
    n = read(b[1], got, sizeof(got));
    CHECK(n == 5, "unexpected byte count on reused fd");
    CHECK(n == 5 && got[0] == 0 && got[1] == 3 &&
          got[2] == 1 && got[3] == 2 && got[4] == 3,
          "reused fd received stale bytes instead of the new frame");

    net_tcp_close(b[0]);
    close(b[1]);
}

/* #8: a slow connection must not block sends to other connections. */
static void test_no_head_of_line_blocking(void)
{
    int slow[2];
    int fast[2];
    uint8_t frame[4] = { 9, 8, 7, 6 };
    uint8_t got[6];
    ssize_t n;

    if (make_pair(slow) != 0 || make_pair(fast) != 0) {
        ++failures;
        return;
    }

    CHECK(fill_until_pending(slow[0]) == 0, "could not get a pending send");

    CHECK(net_tcp_send_frame(fast[0], frame, sizeof(frame)) == 0,
          "send to another fd was refused while one fd has pending data");
    n = read(fast[1], got, sizeof(got));
    CHECK(n == 6 && got[1] == 4 && got[2] == 9,
          "other fd did not receive its frame");

    net_tcp_close(slow[0]);
    close(slow[1]);
    net_tcp_close(fast[0]);
    close(fast[1]);
}

/* Pending data must be delivered intact once the peer reads. */
static void test_pending_flushes_in_order(void)
{
    int p[2];
    uint8_t buf[2048];
    size_t len = 0;
    tcp_recv_state_t st;
    int frames = 0;
    int guard;

    if (make_pair(p) != 0) {
        ++failures;
        return;
    }
    CHECK(fill_until_pending(p[0]) == 0, "could not get a pending send");

    memset(&st, 0, sizeof(st));
    for (guard = 0; guard < 100000 && net_tcp_get_pending_fd() >= 0; ++guard) {
        int rc = net_tcp_recv_frame(p[1], buf, sizeof(buf), &len, &st);
        if (rc == 0) {
            CHECK(len == 1400 && buf[0] == 0xAA && buf[1399] == 0xAA,
                  "corrupted frame");
            ++frames;
        } else if (rc < 0) {
            CHECK(0, "recv_frame failed");
            break;
        }
        net_tcp_flush_pending(p[0]);
    }
    /* Drain whatever is left after the last flush. */
    for (guard = 0; guard < 100000; ++guard) {
        int rc = net_tcp_recv_frame(p[1], buf, sizeof(buf), &len, &st);
        if (rc != 0) {
            break;
        }
        CHECK(len == 1400, "corrupted frame");
        ++frames;
    }
    CHECK(frames > 1, "no frames received");
    CHECK(net_tcp_get_pending_fd() < 0, "pending not flushed");

    net_tcp_close(p[0]);
    close(p[1]);
}

int main(void)
{
    test_close_clears_pending();
    test_no_head_of_line_blocking();
    test_pending_flushes_in_order();

    if (failures != 0) {
        fprintf(stderr, "test_net_tcp: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_net_tcp: OK\n");
    return 0;
}
