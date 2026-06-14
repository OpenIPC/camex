/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * tun.c — TUN device creation and I/O
 *
 */

#define _GNU_SOURCE

#include "tun.h"
#include "util.h"
#include "log.h"
#include "camex.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/if_tun.h>
#includes
#ifndef _WIN32
#include <net/if.h>
#endif
#include <net/if_arp.h>
#include <netinet/in.h>
#include <string.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

int tun_fd = -1;
char tun_name[IFNAMSIZ];

static void copy_ifreq_name(struct ifreq *req, const char *name)
{
    memset(req, 0, sizeof(*req));
    snprintf(req->ifr_name, sizeof(req->ifr_name), "%s", name);
}

static void reset_tun_backend(void)
{
    tun_name[0] = '\0';
}

static void close_tun_creation(int sock, int fd)
{
    if (sock >= 0) {
        close(sock);
    }
    if (fd >= 0) {
        close(fd);
    }
    reset_tun_backend();
}

int tun_create_device(const char *local_ip, const char *netmask,
                      int mtu, const char *tun_dev_override)
{
    struct ifreq ifr;
    struct ifreq cfg;
    struct sockaddr_in *sin;
    int fd = -1;
    int sock;
    int use_tunsetiff = 0;
    const char *devpath_used = NULL;
    static const char DEV_NET_TUN[] = "/dev/net/tun";
    static const char DEV_CAMEX[]   = "/dev/camex";
    const char *p;

    if (local_ip == NULL || netmask == NULL || mtu <= 0) {
        return -1;
    }

    if (tun_dev_override != NULL && tun_dev_override[0] != '\0') {
        devpath_used = tun_dev_override;
        log_message(LOG_INFO, "TUN device override: %s", devpath_used);
        fd = open(devpath_used, O_RDWR);
        if (fd < 0) {
            print_errno_message(LOG_ERR, "open(tun_dev)");
            return -1;
        }
    } else {
        fd = open(DEV_NET_TUN, O_RDWR);
        if (fd >= 0) {
            copy_ifreq_name(&ifr, "tun%d");
            ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
            if (ioctl(fd, TUNSETIFF, (void *)&ifr) >= 0) {
                devpath_used  = DEV_NET_TUN;
                use_tunsetiff = 1;
                snprintf(tun_name, sizeof(tun_name), "%s", ifr.ifr_name);
            } else {
                close(fd);
                fd = -1;
            }
        }

        if (fd < 0) {
            fd = open(DEV_CAMEX, O_RDWR);
            if (fd < 0) {
                log_message(LOG_ERR,
                    "No TUN backend: /dev/net/tun and /dev/camex both failed");
                return -1;
            }
            devpath_used = DEV_CAMEX;
        }
    }

    if (!use_tunsetiff) {
        if (strcmp(devpath_used, DEV_NET_TUN) == 0) {
            copy_ifreq_name(&ifr, "tun%d");
            ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
            if (ioctl(fd, TUNSETIFF, (void *)&ifr) < 0) {
                print_errno_message(LOG_ERR, "ioctl(TUNSETIFF)");
                close_tun_creation(-1, fd);
                return -1;
            }
            snprintf(tun_name, sizeof(tun_name), "%s", ifr.ifr_name);
        } else {
            p = strrchr(devpath_used, '/');
            strncpy(tun_name, (p != NULL) ? p + 1 : devpath_used,
                    sizeof(tun_name) - 1U);
            tun_name[sizeof(tun_name) - 1U] = '\0';
        }
    }

    log_message(LOG_INFO, "TUN backend: %s (%s)", tun_name, devpath_used);

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        print_errno_message(LOG_ERR, "socket(AF_INET, SOCK_DGRAM)");
        close_tun_creation(-1, fd);
        return -1;
    }

    copy_ifreq_name(&cfg, tun_name);
    sin = (struct sockaddr_in *)&cfg.ifr_addr;
    sin->sin_family = AF_INET;
    if (inet_pton(AF_INET, local_ip, &sin->sin_addr) != 1 ||
        ioctl(sock, SIOCSIFADDR, &cfg) < 0) {
        print_errno_message(LOG_ERR, "ioctl(SIOCSIFADDR)");
        close_tun_creation(sock, fd);
        return -1;
    }

    copy_ifreq_name(&cfg, tun_name);
    sin = (struct sockaddr_in *)&cfg.ifr_netmask;
    sin->sin_family = AF_INET;
    if (inet_pton(AF_INET, netmask, &sin->sin_addr) != 1 ||
        ioctl(sock, SIOCSIFNETMASK, &cfg) < 0) {
        print_errno_message(LOG_ERR, "ioctl(SIOCSIFNETMASK)");
        close_tun_creation(sock, fd);
        return -1;
    }

    copy_ifreq_name(&cfg, tun_name);
    cfg.ifr_mtu = mtu;
    if (ioctl(sock, SIOCSIFMTU, &cfg) < 0) {
        print_errno_message(LOG_WARNING, "ioctl(SIOCSIFMTU)");
    }

    copy_ifreq_name(&cfg, tun_name);
    if (ioctl(sock, SIOCGIFFLAGS, &cfg) < 0) {
        print_errno_message(LOG_ERR, "ioctl(SIOCGIFFLAGS)");
        close_tun_creation(sock, fd);
        return -1;
    }

    cfg.ifr_flags |= IFF_UP | IFF_RUNNING;
    if (ioctl(sock, SIOCSIFFLAGS, &cfg) < 0) {
        print_errno_message(LOG_ERR, "ioctl(SIOCSIFFLAGS)");
        close_tun_creation(sock, fd);
        return -1;
    }

    close(sock);
    tun_fd = fd;
    if (set_fd_nonblocking(tun_fd) != 0) {
        print_errno_message(LOG_WARNING, "fcntl(O_NONBLOCK)");
    }
    return 0;
}

void tun_close_device(void)
{
    if (tun_fd >= 0) {
        close(tun_fd);
        tun_fd = -1;
    }
    reset_tun_backend();
}

int tun_read_packet(uint8_t *buffer, size_t size)
{
    ssize_t len;

    if (tun_fd < 0 || buffer == NULL || size == 0U) {
        return -1;
    }

    len = read(tun_fd, buffer, size);
    if (len <= 0 || (size_t)len > size || len > INT_MAX) {
        return -1;
    }

    return (int)len;
}

int tun_write_packet(const uint8_t *buffer, size_t len)
{
    ssize_t written;

    if (tun_fd < 0 || buffer == NULL || len == 0U) {
        return -1;
    }

    written = write(tun_fd, buffer, len);
    if (written < 0 || (size_t)written != len) {
        return -1;
    }

    return 0;
}
