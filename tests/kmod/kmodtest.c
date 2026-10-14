/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * kmodtest.c — runtime test for camex.ko, run inside a QEMU guest by
 * docker/test-kmod-vm.sh. Checks device permissions (#11), the
 * CAP_NET_ADMIN requirement, EBUSY on a second open(), concurrent
 * open(), and the packet path (write() an ICMP echo request, read() the
 * echo reply).
 *
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } else printf("ok: %s\n", m); } while (0)

static uint16_t csum(const void *p, int n)
{
    const uint8_t *b = p; uint32_t s = 0;
    for (; n > 1; n -= 2, b += 2) s += (b[0] << 8) | b[1];
    if (n) s += b[0] << 8;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

static volatile int go;
static int opened[2];
static void *opener(void *arg)
{
    int i = (int)(intptr_t)arg;
    while (!go) { }
    opened[i] = open("/dev/camex", O_RDWR);
    return NULL;
}

int main(void)
{
    struct stat st;
    int fd, fd2, i, both = 0;
    pid_t pid;
    int status;

    CHECK(stat("/dev/camex", &st) == 0, "/dev/camex exists");
    printf("mode: %o\n", st.st_mode & 0777);
    CHECK((st.st_mode & 0077) == 0, "/dev/camex not accessible to group/others");

    /* Unprivileged user, even with a permissive mode, must be refused. */
    chmod("/dev/camex", 0666);
    pid = fork();
    if (pid == 0) {
        if (setgid(1000) || setuid(1000)) _exit(9);
        fd = open("/dev/camex", O_RDWR);
        _exit(fd >= 0 ? 1 : (errno == EPERM ? 0 : 2));
    }
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "uid 1000 open() refused with EPERM even when mode is 0666");
    chmod("/dev/camex", 0600);

    fd = open("/dev/camex", O_RDWR);
    CHECK(fd >= 0, "root open() succeeds");
    fd2 = open("/dev/camex", O_RDWR);
    CHECK(fd2 < 0 && errno == EBUSY, "second open() returns EBUSY");
    close(fd);
    fd = open("/dev/camex", O_RDWR);
    CHECK(fd >= 0, "open() after close() succeeds");
    close(fd);

    /* Concurrent open(): at most one may succeed.  This is a guard, not a
     * reproducer: the old check-then-set window is a few instructions wide
     * and was not hit in 2000 rounds on 4 vCPUs either, so the fix
     * (atomic_cmpxchg) is correct by construction rather than by this test. */
    for (i = 0; i < 2000; i++) {
        pthread_t a, b;
        go = 0; opened[0] = opened[1] = -1;
        pthread_create(&a, NULL, opener, (void *)0);
        pthread_create(&b, NULL, opener, (void *)1);
        go = 1;
        pthread_join(a, NULL); pthread_join(b, NULL);
        if (opened[0] >= 0 && opened[1] >= 0) both++;
        if (opened[0] >= 0) close(opened[0]);
        if (opened[1] >= 0) close(opened[1]);
    }
    printf("concurrent rounds with two successful opens: %d / 2000\n", both);
    CHECK(both == 0, "concurrent open() never succeeds twice");

    /* Packet path: ICMP echo request in via write(), echo reply out via read(). */
    fd = open("/dev/camex", O_RDWR);
    (void)!system("ip addr add 10.9.0.1/24 dev camex; ip link set camex up");
    {
        uint8_t p[28] = {0}, r[2048];
        ssize_t n;
        p[0] = 0x45; p[3] = 28; p[8] = 64; p[9] = 1;
        p[12] = 10; p[13] = 9; p[14] = 0; p[15] = 2;   /* src 10.9.0.2 */
        p[16] = 10; p[17] = 9; p[18] = 0; p[19] = 1;   /* dst 10.9.0.1 */
        { uint16_t c = csum(p, 20); p[10] = c >> 8; p[11] = c & 0xff; }
        p[20] = 8; p[24] = 0x12; p[25] = 0x34; p[27] = 1;
        { uint16_t c = csum(p + 20, 8); p[22] = c >> 8; p[23] = c & 0xff; }
        CHECK(write(fd, p, sizeof(p)) == (ssize_t)sizeof(p), "write() IPv4 echo request");
        {
            int got = 0, k;
            alarm(10);
            for (k = 0; k < 20 && !got; k++) {
                n = read(fd, r, sizeof(r));
                if (n <= 0) break;
                printf("read %zd bytes, first byte 0x%02x\n", n, r[0]);
                if (n >= 28 && (r[0] >> 4) == 4 && r[9] == 1 && r[20] == 0) got = 1;
            }
            alarm(0);
            CHECK(got, "read() returns ICMP echo reply");
        }
    }
    close(fd);

    printf(fails ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", fails);
    return 0;
}
