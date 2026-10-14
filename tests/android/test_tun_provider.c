/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * test_tun_provider.c — runs camex_main() with a TUN provider, the way the
 * Android app does (minus JNI/VpnService), so the native side of the
 * provider path can be tested on Linux: run by docker/test-e2e-tun-provider.sh.
 *
 * The provider logs the parameters it receives, creates a Linux TUN
 * device "prov0" with that address and those routes, and returns its fd.
 *
 */

#include "tun.h"

#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int camex_main(int argc, char *argv[]);

static int provider(const char *local_ip, int prefix_len, int mtu,
                    const char routes[][32], uint8_t route_count, void *ctx)
{
    struct ifreq ifr;
    char cmd[256];
    int fd;
    uint8_t i;

    (void)ctx;
    printf("PROVIDER ip=%s prefix=%d mtu=%d routes=%u", local_ip, prefix_len,
           mtu, (unsigned)route_count);
    for (i = 0; i < route_count; ++i) {
        printf(" %s", routes[i]);
    }
    printf("\n");
    fflush(stdout);

    fd = open("/dev/net/tun", O_RDWR);   /* blocking, like VpnService */
    if (fd < 0) {
        return -1;
    }
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "prov0");
    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        close(fd);
        return -1;
    }
    snprintf(cmd, sizeof(cmd),
             "ip addr add %s/%d dev prov0 && ip link set prov0 mtu %d up",
             local_ip, prefix_len, mtu);
    if (system(cmd) != 0) {
        close(fd);
        return -1;
    }
    for (i = 0; i < route_count; ++i) {
        snprintf(cmd, sizeof(cmd), "ip route add %s dev prov0", routes[i]);
        if (system(cmd) != 0) {
            printf("PROVIDER route failed: %s\n", routes[i]);
        }
    }
    return fd;
}

int main(int argc, char *argv[])
{
    tun_set_provider(provider, NULL);
    return camex_main(argc, argv);
}
