/*
 * camexd - minimal dependency-free UDP/TUN tunnel daemon for embedded targets
 *
 * Usage: camexd -r <remote_ip> [options]
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#define CAMEX_VERSION    "1.0.0"
#define CAMEX_DEFAULT_PORT  7777
#define CAMEX_DEFAULT_TUN   "camex0"
#define CAMEX_DEFAULT_MTU   1400

static volatile sig_atomic_t g_running = 1;
static int g_use_syslog = 0;

static void handle_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

static void log_msg(int priority, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (g_use_syslog) {
        vsyslog(priority, fmt, ap);
    } else {
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
    }
    va_end(ap);
}

/* Open (or create) a TUN interface and return its fd. */
static int tun_open(const char *ifname)
{
    struct ifreq ifr;
    int fd;

    fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) {
        log_msg(LOG_ERR, "open /dev/net/tun: %s", strerror(errno));
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        log_msg(LOG_ERR, "ioctl TUNSETIFF: %s", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

/* Bind a UDP socket to local_addr:local_port. */
static int udp_open(const char *local_addr, int local_port)
{
    struct sockaddr_in addr;
    int fd, one = 1;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        log_msg(LOG_ERR, "socket: %s", strerror(errno));
        return -1;
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)local_port);
    if (local_addr && local_addr[0])
        inet_pton(AF_INET, local_addr, &addr.sin_addr);
    else
        addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_msg(LOG_ERR, "bind :%d: %s", local_port, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "camexd " CAMEX_VERSION " - UDP/TUN tunnel daemon\n\n"
        "Usage: %s -r <remote_addr> [options]\n\n"
        "  -r <addr>   Remote peer IP address (required)\n"
        "  -q <port>   Remote peer UDP port    (default: %d)\n"
        "  -l <addr>   Local bind address      (default: 0.0.0.0)\n"
        "  -p <port>   Local UDP port          (default: %d)\n"
        "  -i <name>   TUN interface name      (default: %s)\n"
        "  -m <mtu>    MTU in bytes            (default: %d)\n"
        "  -d          Daemonize\n"
        "  -v          Verbose (log to stderr)\n"
        "  -h          Show this help\n",
        prog,
        CAMEX_DEFAULT_PORT, CAMEX_DEFAULT_PORT,
        CAMEX_DEFAULT_TUN, CAMEX_DEFAULT_MTU);
}

int main(int argc, char *argv[])
{
    const char *remote_addr = NULL;
    const char *local_addr  = NULL;
    const char *tun_name    = CAMEX_DEFAULT_TUN;
    int remote_port = CAMEX_DEFAULT_PORT;
    int local_port  = CAMEX_DEFAULT_PORT;
    int mtu         = CAMEX_DEFAULT_MTU;
    int do_daemon   = 0;
    int verbose     = 0;
    int opt;

    while ((opt = getopt(argc, argv, "r:q:l:p:i:m:dvh")) != -1) {
        switch (opt) {
        case 'r': remote_addr = optarg;          break;
        case 'q': remote_port = atoi(optarg);    break;
        case 'l': local_addr  = optarg;          break;
        case 'p': local_port  = atoi(optarg);    break;
        case 'i': tun_name    = optarg;          break;
        case 'm': mtu         = atoi(optarg);    break;
        case 'd': do_daemon   = 1;               break;
        case 'v': verbose     = 1;               break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    if (!remote_addr) {
        fprintf(stderr, "error: remote address (-r) is required\n\n");
        usage(argv[0]);
        return 1;
    }

    if (local_port < 1 || local_port > 65535 ||
        remote_port < 1 || remote_port > 65535) {
        fprintf(stderr, "error: port must be in range 1-65535\n");
        return 1;
    }

    if (mtu < 576 || mtu > 65515) {
        fprintf(stderr, "error: mtu must be in range 576-65515\n");
        return 1;
    }

    if (do_daemon) {
        if (daemon(0, 0) < 0) {
            perror("daemon");
            return 1;
        }
        openlog("camexd", LOG_PID | LOG_NDELAY, LOG_DAEMON);
        g_use_syslog = 1;
    }

    signal(SIGTERM, handle_signal);
    signal(SIGINT,  handle_signal);
    signal(SIGHUP,  SIG_IGN);

    int tun_fd = tun_open(tun_name);
    if (tun_fd < 0)
        return 1;

    int udp_fd = udp_open(local_addr, local_port);
    if (udp_fd < 0) {
        close(tun_fd);
        return 1;
    }

    struct sockaddr_in peer;
    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_port   = htons((uint16_t)remote_port);
    if (inet_pton(AF_INET, remote_addr, &peer.sin_addr) != 1) {
        log_msg(LOG_ERR, "invalid remote address: %s", remote_addr);
        close(tun_fd);
        close(udp_fd);
        return 1;
    }

    log_msg(LOG_INFO, "camexd started: tun=%s local=:%d remote=%s:%d mtu=%d",
            tun_name, local_port, remote_addr, remote_port, mtu);

    if (verbose && !g_use_syslog)
        fprintf(stderr, "camexd: tun=%s  local=:%d  remote=%s:%d  mtu=%d\n",
                tun_name, local_port, remote_addr, remote_port, mtu);

    /*
     * Allocate mtu bytes: in a point-to-point tunnel both peers use the
     * same MTU, so neither the TUN read nor the UDP receive will exceed
     * this size.  This keeps heap usage minimal on embedded targets.
     */
    unsigned char *buf = malloc((size_t)mtu);
    if (!buf) {
        log_msg(LOG_ERR, "malloc: %s", strerror(errno));
        close(tun_fd);
        close(udp_fd);
        return 1;
    }

    int maxfd = (tun_fd > udp_fd ? tun_fd : udp_fd) + 1;

    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(tun_fd, &rfds);
        FD_SET(udp_fd, &rfds);

        int ret = select(maxfd, &rfds, NULL, NULL, NULL);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            log_msg(LOG_ERR, "select: %s", strerror(errno));
            break;
        }

        /* TUN → UDP */
        if (FD_ISSET(tun_fd, &rfds)) {
            ssize_t n = read(tun_fd, buf, (size_t)mtu);
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN)
                    continue;
                log_msg(LOG_ERR, "read tun: %s", strerror(errno));
                break;
            }
            if (sendto(udp_fd, buf, (size_t)n, 0,
                       (struct sockaddr *)&peer, sizeof(peer)) < 0)
                log_msg(LOG_WARNING, "sendto: %s", strerror(errno));
        }

        /* UDP → TUN */
        if (FD_ISSET(udp_fd, &rfds)) {
            struct sockaddr_in src;
            socklen_t slen = sizeof(src);
            ssize_t n = recvfrom(udp_fd, buf, (size_t)mtu, 0,
                                 (struct sockaddr *)&src, &slen);
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN)
                    continue;
                log_msg(LOG_ERR, "recvfrom: %s", strerror(errno));
                break;
            }
            if (write(tun_fd, buf, (size_t)n) < 0)
                log_msg(LOG_WARNING, "write tun: %s", strerror(errno));
        }
    }

    log_msg(LOG_INFO, "camexd shutting down");
    free(buf);
    close(tun_fd);
    close(udp_fd);
    if (g_use_syslog)
        closelog();
    return 0;
}
