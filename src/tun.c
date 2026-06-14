/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * tun.c — TUN device creation and I/O (Linux / macOS / Windows WinTUN)
 *
 */

#include "tun.h"
#include "util.h"
#include "log.h"
#include "camex.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
#include <arpa/inet.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#elif defined(__APPLE__)
#include <arpa/inet.h>
#include <net/if.h>
#include <net/if_utun.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/kern_control.h>
/* SYSPROTO_CONTROL may not be in newer macOS SDKs; fallback to value 2 */
#ifndef SYSPROTO_CONTROL
#define SYSPROTO_CONTROL 2
#endif
#endif

#ifdef _WIN32
#include "wintun.h"
#include <process.h>   /* _beginthreadex */
#if !defined(EWOULDBLOCK) && defined(WSAEWOULDBLOCK)
#define EWOULDBLOCK WSAEWOULDBLOCK
#endif
#endif

#ifdef _WIN32
intptr_t tun_fd = -1;
#else
int tun_fd = -1;
#endif
char tun_name[IFNAMSIZ];
static int tun_from_env = 0; /* 1 = fd was passed via CAMEX_TUN_FD */

/* Accept a TUN fd from environment variable CAMEX_TUN_FD (Android VpnService).
 * Returns the accepted fd number or -1 if not set / invalid. */
int tun_accept_fd(void)
{
    const char *env;
    long fd;

    env = getenv("CAMEX_TUN_FD");
    if (env == NULL || *env == '\0') {
        return -1;
    }

    errno = 0;
    fd = strtol(env, NULL, 10);
    if (errno != 0 || fd < 0 || fd != (int)fd) {
        return -1;
    }

    tun_fd = (int)fd;
    tun_from_env = 1;
    snprintf(tun_name, sizeof(tun_name), "camex-android");
    log_message(LOG_INFO, "TUN backend: fd %d from environment (Android VpnService)", (int)fd);
    return tun_fd;
}

#if !defined(_WIN32)
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
#endif /* !_WIN32 */

#if defined(__APPLE__)
/* macOS: create utun interface via kernel control API */
static int tun_open_macos(void)
{
    struct sockaddr_ctl sc;
    struct ctl_info ctlInfo;
    socklen_t optlen;
    int unit;
    int fd;

    memset(&ctlInfo, 0, sizeof(ctlInfo));
    snprintf(ctlInfo.ctl_name, sizeof(ctlInfo.ctl_name), "%s",
             UTUN_CONTROL_NAME);

    fd = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
    if (fd < 0) {
        print_errno_message(LOG_ERR, "socket(PF_SYSTEM) for utun");
        return -1;
    }

    if (ioctl(fd, CTLIOCGINFO, &ctlInfo) < 0) {
        print_errno_message(LOG_ERR, "ioctl(CTLIOCGINFO) for utun");
        close(fd);
        return -1;
    }

    sc.sc_id = ctlInfo.ctl_id;
    sc.sc_len = sizeof(sc);
    sc.sc_family = AF_SYSTEM;
    sc.ss_sysaddr = SYSPROTO_CONTROL;
    sc.sc_unit = 0;  /* 0 = dynamic allocation */

    if (connect(fd, (struct sockaddr *)&sc, sizeof(sc)) < 0) {
        print_errno_message(LOG_ERR, "connect(utun)");
        close(fd);
        return -1;
    }

    /* Get the assigned unit number */
    optlen = sizeof(sc);
    if (getsockname(fd, (struct sockaddr *)&sc, &optlen) == 0) {
        unit = sc.sc_unit;
    } else {
        unit = 0;
    }

    snprintf(tun_name, sizeof(tun_name), "utun%d", unit);
    log_message(LOG_INFO, "TUN backend: %s (macOS utun)", tun_name);
    return fd;
}

/* macOS: configure interface IP/MTU and bring it up */
static int tun_configure_macos(int fd)
{
    struct ifreq cfg;
    struct sockaddr_in *sin;
    char local_ip[16];
    char netmask[16];
    int sock;
    int ret = -1;

    if (parse_local_cidr(current_config.local_cidr, local_ip,
                         sizeof(local_ip), netmask,
                         sizeof(netmask)) != 0) {
        return -1;
    }

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        print_errno_message(LOG_ERR, "socket for ifconfig");
        return -1;
    }

    copy_ifreq_name(&cfg, tun_name);
    sin = (struct sockaddr_in *)&cfg.ifr_addr;
    sin->sin_family = AF_INET;
    if (inet_pton(AF_INET, local_ip, &sin->sin_addr) != 1 ||
        ioctl(sock, SIOCSIFADDR, &cfg) < 0) {
        print_errno_message(LOG_ERR, "ioctl(SIOCSIFADDR)");
        goto done;
    }

    copy_ifreq_name(&cfg, tun_name);
    sin = (struct sockaddr_in *)&cfg.ifr_addr;
    sin->sin_family = AF_INET;
    if (inet_pton(AF_INET, netmask, &sin->sin_addr) != 1 ||
        ioctl(sock, SIOCSIFNETMASK, &cfg) < 0) {
        print_errno_message(LOG_ERR, "ioctl(SIOCSIFNETMASK)");
        goto done;
    }

    copy_ifreq_name(&cfg, tun_name);
    cfg.ifr_mtu = current_config.mtu;
    if (ioctl(sock, SIOCSIFMTU, &cfg) < 0) {
        print_errno_message(LOG_WARNING, "ioctl(SIOCSIFMTU)");
    }

    copy_ifreq_name(&cfg, tun_name);
    if (ioctl(sock, SIOCGIFFLAGS, &cfg) < 0) {
        print_errno_message(LOG_ERR, "ioctl(SIOCGIFFLAGS)");
        goto done;
    }

    cfg.ifr_flags |= IFF_UP | IFF_RUNNING;
    if (ioctl(sock, SIOCSIFFLAGS, &cfg) < 0) {
        print_errno_message(LOG_ERR, "ioctl(SIOCSIFFLAGS)");
        goto done;
    }

    ret = 0;
done:
    close(sock);
    return ret;
}
#endif /* __APPLE__ */

#ifdef _WIN32
/* WinTUN reader thread: waits for packets from WinTUN and forwards
 * them to a UDP signal socket so the main select() loop can work. */
struct wintun_thread_arg {
    WINTUN_SESSION_HANDLE sess;
    SOCKET signal_sock;
    struct sockaddr_in signal_dest;
    volatile int *running;
};

static unsigned int __stdcall wintun_reader_thread(void *arg)
{
    struct wintun_thread_arg *a = (struct wintun_thread_arg *)arg;
    HANDLE wait_ev;
    BYTE *packet;
    DWORD packet_size;
    int ret;
    WINTUN_SESSION_HANDLE sess = a->sess;
    SOCKET sock = a->signal_sock;
    struct sockaddr_in dest = a->signal_dest;
    volatile int *thread_running = a->running;

    wait_ev = pWintunGetReadWaitEvent(sess);
    if (wait_ev == NULL) {
        log_message(LOG_ERR, "WinTUN: WintunGetReadWaitEvent failed");
        return 1;
    }

    while (*thread_running) {
        ret = WaitForSingleObject(wait_ev, 1000);
        if (ret == WAIT_TIMEOUT) {
            continue;
        }
        if (!*thread_running) {
            break;
        }
        /* Drain all available packets */
        for (;;) {
            packet = pWintunReceivePacket(sess, &packet_size);
            if (packet == NULL) {
                break;  /* no more packets */
            }
            /* Forward to UDP signal socket for main loop */
            sendto(sock, (const char *)packet, (int)packet_size, 0,
                   (const struct sockaddr *)&dest, sizeof(dest));
            pWintunReleaseReceivePacket(sess, packet);
        }
    }
    return 0;
}
#endif /* _WIN32 */

#ifdef _WIN32
/* Use netsh for IP/MTU config — avoids dependency on MIB_* structs
 * that are missing in older MinGW-w64 headers. */
static int set_ip_windows(const char *adapter_name,
                          const char *local_ip, const char *netmask_str,
                          int mtu)
{
    char cmd[512];
    int ret;

    /* Set IP, netmask, and bring interface up */
    snprintf(cmd, sizeof(cmd),
             "netsh interface ip set address name=\"%s\" "
             "static %s %s >nul 2>&1",
             adapter_name, local_ip, netmask_str);
    ret = system(cmd);
    if (ret != 0) {
        log_message(LOG_ERR,
                    "WinTUN: netsh set address failed (%d)", ret);
        /* Continue anyway — interface may already have an address */
    }

    /* Set MTU */
    snprintf(cmd, sizeof(cmd),
             "netsh interface ipv4 set subinterface \"%s\" mtu=%d "
             "store=persist >nul 2>&1",
             adapter_name, mtu);
    ret = system(cmd);
    if (ret != 0) {
        log_message(LOG_WARNING,
                    "WinTUN: netsh set MTU failed (%d)", ret);
    }

    return 0;
}
#endif

int tun_create_device(const char *local_ip, const char *netmask,
                      int mtu, const char *tun_dev_override)
{
    (void)local_ip;
    (void)netmask;
    (void)mtu;

#if defined(_WIN32)
    (void)tun_dev_override;
    WINTUN_ADAPTER_HANDLE adapter;
    SOCKET signal_sock;
    struct sockaddr_in signal_addr;
    int socklen;
    HANDLE thread_h;
    unsigned int thread_id;
    struct wintun_thread_arg *targ;

    if (local_ip == NULL || netmask == NULL || mtu <= 0) {
        return -1;
    }

    /* Load wintun.dll */
    if (wintun_load_dll() != 0) {
        log_message(LOG_ERR, "WinTUN: failed to load wintun.dll");
        return -1;
    }

    /* Create adapter */
    adapter = pWintunCreateAdapter(L"Camex", L"Camex", NULL);
    if (adapter == NULL) {
        log_message(LOG_ERR, "WinTUN: WintunCreateAdapter failed (%lu)",
                    (unsigned long)GetLastError());
        wintun_unload_dll();
        return -1;
    }
    g_wintun_adapter = adapter;
    log_message(LOG_INFO, "WinTUN: adapter 'Camex' created");

    /* Set IP address, netmask, MTU */
    if (set_ip_windows("Camex", local_ip, netmask, mtu) != 0) {
        log_message(LOG_ERR, "WinTUN: failed to configure IP/MTU");
        pWintunCloseAdapter(adapter);
        g_wintun_adapter = NULL;
        wintun_unload_dll();
        return -1;
    }

    /* Start session */
    g_wintun_sess = pWintunStartSession(adapter, WINTUN_MIN_RING_CAPACITY);
    if (g_wintun_sess == NULL) {
        log_message(LOG_ERR, "WinTUN: WintunStartSession failed (%lu)",
                    (unsigned long)GetLastError());
        pWintunCloseAdapter(adapter);
        g_wintun_adapter = NULL;
        wintun_unload_dll();
        return -1;
    }

    /* Create UDP signal socket for the reader thread */
    signal_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (signal_sock == INVALID_SOCKET) {
        log_message(LOG_ERR, "WinTUN: signal socket creation failed (%lu)",
                    (unsigned long)WSAGetLastError());
        pWintunEndSession(g_wintun_sess);
        g_wintun_sess = NULL;
        pWintunCloseAdapter(adapter);
        g_wintun_adapter = NULL;
        wintun_unload_dll();
        return -1;
    }

    /* Bind to localhost:0 */
    memset(&signal_addr, 0, sizeof(signal_addr));
    signal_addr.sin_family = AF_INET;
    signal_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    signal_addr.sin_port = 0;
    if (bind(signal_sock, (struct sockaddr *)&signal_addr,
             sizeof(signal_addr)) < 0) {
        log_message(LOG_ERR, "WinTUN: signal socket bind failed (%lu)",
                    (unsigned long)WSAGetLastError());
        closesocket(signal_sock);
        pWintunEndSession(g_wintun_sess);
        g_wintun_sess = NULL;
        pWintunCloseAdapter(adapter);
        g_wintun_adapter = NULL;
        wintun_unload_dll();
        return -1;
    }

    /* Get the port we bound to */
    socklen = sizeof(signal_addr);
    getsockname(signal_sock, (struct sockaddr *)&signal_addr, &socklen);

    /* Start the reader thread */
    g_wintun_running = 1;
    targ = (struct wintun_thread_arg *)malloc(sizeof(*targ));
    if (targ == NULL) {
        closesocket(signal_sock);
        pWintunEndSession(g_wintun_sess);
        g_wintun_sess = NULL;
        pWintunCloseAdapter(adapter);
        g_wintun_adapter = NULL;
        wintun_unload_dll();
        return -1;
    }
    targ->sess = g_wintun_sess;
    targ->signal_sock = signal_sock;
    targ->signal_dest = signal_addr;
    targ->running = &g_wintun_running;

    thread_h = (HANDLE)_beginthreadex(NULL, 0, wintun_reader_thread,
                                       targ, 0, &thread_id);
    if (thread_h == NULL) {
        log_message(LOG_ERR, "WinTUN: failed to start reader thread");
        free(targ);
        closesocket(signal_sock);
        pWintunEndSession(g_wintun_sess);
        g_wintun_sess = NULL;
        pWintunCloseAdapter(adapter);
        g_wintun_adapter = NULL;
        wintun_unload_dll();
        return -1;
    }
    g_wintun_thread = thread_h;

    tun_fd = (intptr_t)signal_sock;
    snprintf(tun_name, sizeof(tun_name), "wintun0");
    log_message(LOG_INFO, "TUN backend: wintun (session %p, signal fd %" PRIdPTR ")",
                (void *)g_wintun_sess, tun_fd);
    return 0;
#elif defined(__APPLE__)
    int fd;

    if (local_ip == NULL || netmask == NULL || mtu <= 0) {
        return -1;
    }

    /* macOS: use utun kernel control */
    fd = tun_open_macos();
    if (fd < 0) {
        log_message(LOG_ERR, "Failed to create utun interface");
        return -1;
    }

    /* Configure IP/MTU/UP */
    if (tun_configure_macos(fd) != 0) {
        close_tun_creation(-1, fd);
        return -1;
    }

    tun_fd = fd;
    if (set_fd_nonblocking(tun_fd) != 0) {
        print_errno_message(LOG_WARNING, "fcntl(O_NONBLOCK)");
    }
    return 0;
#else
    /* Linux: original implementation */
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
#endif
}

void tun_close_device(void)
{
#if defined(_WIN32)
    /* Stop WinTUN reader thread */
    g_wintun_running = 0;
    if (g_wintun_thread != NULL) {
        WaitForSingleObject(g_wintun_thread, 3000);
        CloseHandle(g_wintun_thread);
        g_wintun_thread = NULL;
    }
    /* Close WinTUN session */
    if (g_wintun_sess != NULL) {
        pWintunEndSession(g_wintun_sess);
        g_wintun_sess = NULL;
    }
    /* Destroy WinTUN adapter */
    if (g_wintun_adapter != NULL) {
        pWintunDeleteAdapter(g_wintun_adapter, TRUE);
        pWintunCloseAdapter(g_wintun_adapter);
        g_wintun_adapter = NULL;
    }
    /* Close signal socket */
    if (tun_fd >= 0) {
        closesocket((SOCKET)tun_fd);
        tun_fd = -1;
    }
    wintun_unload_dll();
#else
    if (tun_fd >= 0) {
        close((int)tun_fd);
        tun_fd = -1;
    }
    reset_tun_backend();
#endif
}

ssize_t tun_read_packet(uint8_t *buffer, size_t size)
{
#if defined(__APPLE__)
    /* macOS utun: first 4 bytes are protocol family (AF_INET = 2) */
    uint8_t raw[TUN_PACKET_MAX + 4];
    ssize_t len;

    if (tun_fd < 0 || buffer == NULL || size == 0U) {
        return -1;
    }

    len = read(tun_fd, raw, sizeof(raw));
    if (len <= 4) {
        return -1;
    }

    len -= 4;
    if ((size_t)len > size) {
        return -1;
    }

    memcpy(buffer, raw + 4, (size_t)len);
    return len;
#elif defined(_WIN32)
    /* Windows WinTUN: read from UDP signal socket */
    ssize_t len;

    if (tun_fd < 0 || buffer == NULL || size == 0U) {
        return -1;
    }

    {
        int addr_len = 0;
        len = recvfrom((SOCKET)tun_fd, (char *)buffer, (int)size, 0,
                       NULL, &addr_len);
    }
    return (len < 0) ? -1 : len;
#else
    ssize_t len;

    if (tun_fd < 0 || buffer == NULL || size == 0U) {
        return -1;
    }

    len = read(tun_fd, buffer, size);
    return len;
#endif
}

int tun_write_packet(const uint8_t *buffer, size_t len)
{
    if (tun_fd < 0 || buffer == NULL || len == 0U) {
        return -1;
    }

#if defined(__APPLE__)
    /* macOS utun: prepend 4-byte protocol family header */
    {
        uint8_t raw[TUN_PACKET_MAX + 4];
        uint32_t family = htonl(AF_INET);
        ssize_t written;

        if (len + 4 > sizeof(raw)) {
            return -1;
        }

        memcpy(raw, &family, 4);
        memcpy(raw + 4, buffer, len);
        written = write(tun_fd, raw, len + 4);
        if (written < 0 || (size_t)written != len + 4) {
            return -1;
        }
    }
#elif defined(_WIN32)
    /* Windows WinTUN: allocate and send */
    {
        BYTE *packet;

        if (g_wintun_sess == NULL) {
            return -1;
        }

        packet = pWintunAllocateSendPacket(g_wintun_sess, (DWORD)len);
        if (packet == NULL) {
            return -1;
        }

        memcpy(packet, buffer, len);
        pWintunSendPacket(g_wintun_sess, packet);
    }
#else
    {
        ssize_t written;

        written = write(tun_fd, buffer, len);
        if (written < 0 || (size_t)written != len) {
            return -1;
        }
    }
#endif

    return 0;
}
