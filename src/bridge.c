/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * bridge.c — a TAP network bridged to a host interface without a bridge
 *            device (bridge.h)
 *
 */

#include "bridge.h"
#include "l2.h"
#include "log.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__linux__)
#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PACKET_VNET_HDR
#define PACKET_VNET_HDR 15
#endif

/* struct virtio_net_hdr, in host byte order */
typedef struct {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} vnet_hdr_t;

#define VNET_F_NEEDS_CSUM 1U
#define VNET_GSO_NONE  0U
#define VNET_GSO_TCPV4 1U
#define VNET_GSO_UDP   3U
#define VNET_GSO_TCPV6 4U
#define VNET_GSO_ECN   0x80U

#define BRIDGE_REMOTES 256U
#define BRIDGE_REMOTE_AGE 600      /* seconds a remote MAC stays known */
#define BRIDGE_CLAIM_RECHECK 10    /* a refused address claim, asked again */
#define BRIDGE_PROBE_EVERY 5
#define BRIDGE_PEER_HOLD 15        /* a peer bridge counts this long */
#define BRIDGE_FRAME_MAX 65600U    /* a TSO/GRO frame and its vnet header */

typedef struct {
    uint8_t mac[6];
    uint32_t ip_be;          /* its IPv4 address, 0 = not seen yet */
    uint32_t neigh_ip_be;    /* the neighbour entry made for it */
    uint32_t refused_ip_be;  /* a LAN device's address it claimed (#206) */
    time_t refused_at;       /* when; asked again BRIDGE_CLAIM_RECHECK on */
    uint8_t probed;          /* a stale entry for it was probed by ARP */
    time_t seen;
    uint8_t used;
} bridge_remote_t;

static struct {
    int ready;               /* the fields below set up (bridge_open) */
    int open;
    int pkt;                 /* AF_PACKET socket */
    int raw;                 /* AF_INET raw socket: packets to the host */
    int nl;                  /* netlink: neighbour entries */
    int vnet;                /* PACKET_VNET_HDR is on */
    int nat;
    int ifindex;
    char ifname[IFNAMSIZ];
    uint8_t host_mac[6];
    uint32_t host_ip_be;
    uint32_t host_mask_be;
    int mtu;
    uint8_t probe_tag[8];    /* the network's, in probes */
    char client_id[64];
    bridge_send_fn to_tunnel;
    time_t last_probe;
    time_t last_host_check;
    time_t peer_until;       /* a lower-ID bridge of the network is here */
    time_t listen_until;     /* just started: listening for other bridges */
    char peer_id[65];
    unsigned int nl_seq;
    uint8_t warned_big;
    uint8_t warned_forward;
    bridge_remote_t remotes[BRIDGE_REMOTES];
} br;   /* all zero (bss, #204): its sockets mean something once ready */

static const uint8_t mac_bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* ---------------------------------------------------------------- */
/* Checksums and segmentation                                        */
/* ---------------------------------------------------------------- */

static uint32_t sum16(const uint8_t *p, size_t len, uint32_t sum)
{
    while (len > 1U) {
        sum += (uint32_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2U;
    }
    if (len == 1U) {
        sum += (uint32_t)(p[0] << 8);
    }
    return sum;
}

static uint16_t fold(uint32_t sum)
{
    while ((sum >> 16) != 0U) {
        sum = (sum & 0xFFFFU) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static size_t l3_offset(const uint8_t *frame, size_t len, uint16_t *type)
{
    size_t off = 12U;
    uint16_t t;

    if (len < L2_ETH_HDR) {
        return 0U;
    }
    t = (uint16_t)((frame[off] << 8) | frame[off + 1U]);
    if (t == 0x8100U && len >= L2_ETH_HDR + 4U) {   /* one VLAN tag */
        off += 4U;
        t = (uint16_t)((frame[off] << 8) | frame[off + 1U]);
    }
    *type = t;
    return off + 2U;
}

/* The L4 checksum of an IPv4/IPv6 TCP or UDP packet at frame+l3, all of
 * it computed (a partial one from checksum offload included) */
int bridge_fix_checksum(uint8_t *frame, size_t len, size_t l3)
{
    uint8_t *ip = frame + l3;
    size_t iplen;
    size_t l4;
    uint8_t proto;
    uint32_t sum = 0U;
    size_t seg;
    uint8_t *ck;

    if (l3 >= len) {
        return -1;
    }
    iplen = len - l3;
    if ((ip[0] >> 4) == 4U) {
        size_t ihl = (size_t)(ip[0] & 0x0FU) * 4U;
        size_t tot = (size_t)((ip[2] << 8) | ip[3]);

        if (ihl < 20U || tot < ihl || tot > iplen) {
            return -1;
        }
        if ((((ip[6] << 8) | ip[7]) & 0x3FFFU) != 0U) {
            return 0;   /* a fragment: nothing to do here */
        }
        proto = ip[9];
        l4 = ihl;
        seg = tot - ihl;
        sum = sum16(ip + 12, 8U, 0U);
        sum += proto;
        sum += (uint32_t)seg;
        ip[10] = ip[11] = 0U;
        {
            uint16_t c = fold(sum16(ip, ihl, 0U));

            ip[10] = (uint8_t)(c >> 8);
            ip[11] = (uint8_t)c;
        }
    } else if ((ip[0] >> 4) == 6U) {
        size_t plen;

        if (iplen < 40U) {
            return -1;
        }
        plen = (size_t)((ip[4] << 8) | ip[5]);
        proto = ip[6];
        if (plen + 40U > iplen) {
            return -1;
        }
        l4 = 40U;
        seg = plen;
        sum = sum16(ip + 8, 32U, 0U);
        sum += (uint32_t)seg;
        sum += proto;
    } else {
        return -1;
    }
    if (proto == 6U && seg >= 20U) {
        ck = ip + l4 + 16;
    } else if (proto == 17U && seg >= 8U) {
        ck = ip + l4 + 6;
    } else {
        return 0;
    }
    ck[0] = ck[1] = 0U;
    {
        uint16_t c = fold(sum16(ip + l4, seg, sum));

        if (proto == 17U && c == 0U) {
            c = 0xFFFFU;
        }
        ck[0] = (uint8_t)(c >> 8);
        ck[1] = (uint8_t)c;
    }
    return 0;
}

int bridge_segment(const uint8_t *frame, size_t len, size_t mss,
                   int (*send)(const uint8_t *frame, size_t len))
{
    static uint8_t out[L2_ETH_HDR + 4U + 60U + 60U + 9216U];
    uint16_t type;
    size_t l3 = l3_offset(frame, len, &type);
    const uint8_t *ip = frame + l3;
    size_t l3len;
    size_t tcp;
    size_t hdrs;
    size_t payload;
    size_t off;
    uint32_t seq;
    uint16_t id = 0U;
    int v4;

    if (l3 == 0U || mss == 0U || mss > 9000U) {
        return -1;
    }
    if (type == 0x0800U && len >= l3 + 20U && ip[9] == 6U) {
        v4 = 1;
        l3len = (size_t)(ip[0] & 0x0FU) * 4U;
        id = (uint16_t)((ip[4] << 8) | ip[5]);
    } else if (type == 0x86DDU && len >= l3 + 40U && ip[6] == 6U) {
        v4 = 0;
        l3len = 40U;
    } else {
        return -1;
    }
    tcp = l3 + l3len;
    if (len < tcp + 20U) {
        return -1;
    }
    hdrs = tcp + (size_t)(frame[tcp + 12U] >> 4) * 4U;
    if (hdrs > len || hdrs - l3 > 120U) {
        return -1;
    }
    payload = len - hdrs;
    seq = ((uint32_t)frame[tcp + 4U] << 24) | ((uint32_t)frame[tcp + 5U] << 16) |
          ((uint32_t)frame[tcp + 6U] << 8) | frame[tcp + 7U];
    for (off = 0U; off < payload || (payload == 0U && off == 0U);
         off += mss) {
        size_t n = payload - off < mss ? payload - off : mss;
        size_t total = hdrs + n;
        uint8_t *o_ip = out + l3;
        uint8_t *o_tcp = out + tcp;
        uint32_t s = seq + (uint32_t)off;
        int last = off + n >= payload;

        memcpy(out, frame, hdrs);
        memcpy(out + hdrs, frame + hdrs + off, n);
        if (v4) {
            size_t tot = total - l3;
            uint16_t sid = (uint16_t)(id + off / mss);

            o_ip[2] = (uint8_t)(tot >> 8);
            o_ip[3] = (uint8_t)tot;
            o_ip[4] = (uint8_t)(sid >> 8);
            o_ip[5] = (uint8_t)sid;
        } else {
            size_t plen = total - l3 - 40U;

            o_ip[4] = (uint8_t)(plen >> 8);
            o_ip[5] = (uint8_t)plen;
        }
        o_tcp[4] = (uint8_t)(s >> 24);
        o_tcp[5] = (uint8_t)(s >> 16);
        o_tcp[6] = (uint8_t)(s >> 8);
        o_tcp[7] = (uint8_t)s;
        if (!last) {
            o_tcp[13] &= (uint8_t)~0x09U;   /* FIN and PSH: the last only */
        }
        if (off != 0U) {
            o_tcp[13] &= (uint8_t)~0x80U;   /* CWR: the first only */
        }
        if (bridge_fix_checksum(out, total, l3) != 0 ||
            send(out, total) != 0) {
            return -1;
        }
        if (payload == 0U) {
            break;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* Remote devices, neighbour entries                                 */
/* ---------------------------------------------------------------- */

static bridge_remote_t *remote_find(const uint8_t mac[6])
{
    size_t i;

    for (i = 0U; i < BRIDGE_REMOTES; ++i) {
        if (br.remotes[i].used && memcmp(br.remotes[i].mac, mac, 6) == 0 &&
            difftime(time(NULL), br.remotes[i].seen) <=
                (double)BRIDGE_REMOTE_AGE) {
            return &br.remotes[i];
        }
    }
    return NULL;
}

static void neigh_set(uint32_t ip_be, const uint8_t mac[6], int add);

/* A remote device not heard for BRIDGE_REMOTE_AGE: forgotten, and its
 * neighbour entry with it */
static void remote_expire(bridge_remote_t *r)
{
    size_t i;
    int shared = 0;

    /* Its entry only when no live remote has that address now: a newer one
     * may have replaced the kernel's entry with its own MAC */
    for (i = 0U; i < BRIDGE_REMOTES; ++i) {
        if (&br.remotes[i] != r && br.remotes[i].used &&
            br.remotes[i].neigh_ip_be == r->neigh_ip_be) {
            shared = 1;
        }
    }
    if (r->neigh_ip_be != 0U && br.ifindex != 0 && !shared) {
        neigh_set(r->neigh_ip_be, r->mac, 0);
    }
    r->neigh_ip_be = 0U;
    r->ip_be = 0U;
    r->used = 0U;
}

static bridge_remote_t *remote_by_ip(uint32_t ip_be)
{
    size_t i;

    for (i = 0U; i < BRIDGE_REMOTES; ++i) {
        bridge_remote_t *r = &br.remotes[i];

        if (!r->used || r->ip_be != ip_be || ip_be == 0U) {
            continue;
        }
        /* Gone (or its address now another's): not sent to any more */
        if (difftime(time(NULL), r->seen) > (double)BRIDGE_REMOTE_AGE) {
            remote_expire(r);
            continue;
        }
        return r;
    }
    return NULL;
}

/* A permanent neighbour entry ip -> mac on the interface (add), or its
 * removal */
static void neigh_set(uint32_t ip_be, const uint8_t mac[6], int add)
{
    struct {
        struct nlmsghdr nh;
        struct ndmsg nd;
        char attrs[64];
    } req;
    struct rtattr *ra;
    struct sockaddr_nl to;

    if (br.nl < 0) {
        br.nl = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
        if (br.nl < 0) {
            return;
        }
    }
    memset(&req, 0, sizeof(req));
    req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ndmsg));
    req.nh.nlmsg_type = add ? RTM_NEWNEIGH : RTM_DELNEIGH;
    req.nh.nlmsg_flags = (uint16_t)(NLM_F_REQUEST |
                                    (add ? NLM_F_CREATE | NLM_F_REPLACE : 0));
    req.nh.nlmsg_seq = ++br.nl_seq;
    req.nd.ndm_family = AF_INET;
    req.nd.ndm_ifindex = br.ifindex;
    req.nd.ndm_state = NUD_PERMANENT;
    ra = (struct rtattr *)((char *)&req + NLMSG_ALIGN(req.nh.nlmsg_len));
    ra->rta_type = NDA_DST;
    ra->rta_len = RTA_LENGTH(4);
    memcpy(RTA_DATA(ra), &ip_be, 4);
    req.nh.nlmsg_len = NLMSG_ALIGN(req.nh.nlmsg_len) + RTA_ALIGN(ra->rta_len);
    if (add) {
        ra = (struct rtattr *)((char *)&req + NLMSG_ALIGN(req.nh.nlmsg_len));
        ra->rta_type = NDA_LLADDR;
        ra->rta_len = RTA_LENGTH(6);
        memcpy(RTA_DATA(ra), mac, 6);
        req.nh.nlmsg_len = NLMSG_ALIGN(req.nh.nlmsg_len) +
                           RTA_ALIGN(ra->rta_len);
    }
    memset(&to, 0, sizeof(to));
    to.nl_family = AF_NETLINK;
    (void)sendto(br.nl, &req, req.nh.nlmsg_len, MSG_DONTWAIT,
                 (struct sockaddr *)&to, sizeof(to));
}

/* The host's neighbour entry for ip_be on the interface: 1 and its state
 * and MAC when there is one with a MAC, 0 when the whole table was read
 * and there is none, -1 when it could not be read (#212).
 * A dump, filtered here: RTM_GETNEIGH of one address needs Linux 4.20. */
static int neigh_get(uint32_t ip_be, uint16_t *state, uint8_t mac[6])
{
    struct {
        struct nlmsghdr nh;
        struct ndmsg nd;
    } req;
    struct sockaddr_nl to;
    struct timeval tv;
    static uint8_t buf[16384];
    int fd, found = 0, done = 0, failed = 0, rounds = 0;

    fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0) {
        return -1;
    }
    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    memset(&req, 0, sizeof(req));
    req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ndmsg));
    req.nh.nlmsg_type = RTM_GETNEIGH;
    req.nh.nlmsg_flags = (uint16_t)(NLM_F_REQUEST | NLM_F_DUMP);
    req.nh.nlmsg_seq = ++br.nl_seq;
    req.nd.ndm_family = AF_INET;
    req.nd.ndm_ifindex = br.ifindex;
    memset(&to, 0, sizeof(to));
    to.nl_family = AF_NETLINK;
    if (sendto(fd, &req, req.nh.nlmsg_len, 0, (struct sockaddr *)&to,
               sizeof(to)) < 0) {
        close(fd);
        return -1;
    }
    while (!done && rounds++ < 64) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        struct nlmsghdr *nh;
        size_t left;

        if (n <= 0) {
            break;
        }
        left = (size_t)n;
        for (nh = (struct nlmsghdr *)buf; NLMSG_OK(nh, left);
             nh = NLMSG_NEXT(nh, left)) {
            struct ndmsg *nd;
            struct rtattr *ra;
            size_t alen;
            const uint8_t *dst = NULL, *ll = NULL;

            if (nh->nlmsg_seq != req.nh.nlmsg_seq) {
                continue;
            }
            if (nh->nlmsg_type == NLMSG_DONE) {
                done = 1;
                break;
            }
            if (nh->nlmsg_type == NLMSG_ERROR) {
                done = failed = 1;
                break;
            }
            if (nh->nlmsg_type != RTM_NEWNEIGH ||
                nh->nlmsg_len < NLMSG_LENGTH(sizeof(*nd))) {
                continue;
            }
            nd = (struct ndmsg *)NLMSG_DATA(nh);
            if (nd->ndm_family != AF_INET || nd->ndm_ifindex != br.ifindex) {
                continue;
            }
            alen = nh->nlmsg_len - NLMSG_LENGTH(sizeof(*nd));
            for (ra = (struct rtattr *)((char *)nd + NLMSG_ALIGN(sizeof(*nd)));
                 RTA_OK(ra, alen); ra = RTA_NEXT(ra, alen)) {
                if (ra->rta_type == NDA_DST && RTA_PAYLOAD(ra) == 4U) {
                    dst = (const uint8_t *)RTA_DATA(ra);
                } else if (ra->rta_type == NDA_LLADDR &&
                           RTA_PAYLOAD(ra) == 6U) {
                    ll = (const uint8_t *)RTA_DATA(ra);
                }
            }
            if (dst != NULL && ll != NULL && memcmp(dst, &ip_be, 4) == 0) {
                *state = nd->ndm_state;
                memcpy(mac, ll, 6);
                found = 1;
            }
        }
    }
    close(fd);
    return failed || !done ? -1 : found;
}

/* May a remote's claim of ip_be become the host's neighbour entry (#206)?
 * 1 yes, 0 no (a LAN device's), -1 not yet (a stale entry to probe first),
 * -2 not now (the host's table could not be read: asked again later).
 * Not when the host has an entry for it with another MAC that it learned
 * itself from the LAN, or that someone else made: that address is a LAN
 * device's, and the tunnel's claim would take its traffic. One of camex's
 * own (a remote's MAC) or one that never resolved may be replaced. */
static int neigh_may_take(uint32_t ip_be, const uint8_t mac[6], int probed)
{
    uint16_t state = 0U;
    uint8_t had[6];
    size_t i;
    int got = neigh_get(ip_be, &state, had);

    if (got < 0) {
        return -2;
    }
    if (got == 0 || memcmp(had, mac, 6) == 0) {
        return 1;
    }
    for (i = 0U; i < BRIDGE_REMOTES; ++i) {
        if (br.remotes[i].used && br.remotes[i].neigh_ip_be == ip_be &&
            memcmp(br.remotes[i].mac, had, 6) == 0) {
            return 1;
        }
    }
    if ((state & (NUD_REACHABLE | NUD_DELAY | NUD_PROBE | NUD_PERMANENT |
                  NUD_NOARP)) != 0U) {
        return 0;
    }
    /* Stale: maybe a device gone (its address handed out again by DHCP),
     * maybe one just quiet. Asked once by ARP (claim_probe); still stale at
     * the next check, nobody answered and the claim stands. */
    return (state & NUD_STALE) == 0U || probed ? 1 : -1;
}

static int wire_send(const uint8_t *frame, size_t len);

/* An ARP request from the host for ip_be on the LAN: a device that has
 * the address answers the host, whose entry then turns reachable.
 * 0 when it went out. */
static int claim_probe(uint32_t ip_be)
{
    uint8_t f[60];

    memset(f, 0, sizeof(f));
    memcpy(f, mac_bcast, 6);
    memcpy(f + 6, br.host_mac, 6);
    f[12] = 0x08U;
    f[13] = 0x06U;
    f[15] = 1U;                      /* Ethernet */
    f[16] = 0x08U;                   /* IPv4 */
    f[18] = 6U;
    f[19] = 4U;
    f[21] = 1U;                      /* request */
    memcpy(f + 22, br.host_mac, 6);
    memcpy(f + 28, &br.host_ip_be, 4);
    memcpy(f + 38, &ip_be, 4);
    return wire_send(f, sizeof(f)) < 0 ? -1 : 0;
}

/* Is ip_be on the host's LAN (its interface's subnet)? */
static int on_lan(uint32_t ip_be)
{
    return br.host_mask_be != 0U && ip_be != 0U && ip_be != br.host_ip_be &&
           (ip_be & br.host_mask_be) == (br.host_ip_be & br.host_mask_be);
}

/* A frame from the tunnel: its source is a remote device; its IPv4
 * address (ARP sender, IPv4 source) gives the host a neighbour entry */
static bridge_remote_t *remote_learn(const uint8_t *frame, size_t len)
{
    const uint8_t *src = frame + 6;
    bridge_remote_t *r = remote_find(src);
    uint16_t type;
    size_t l3 = l3_offset(frame, len, &type);
    uint32_t ip = 0U;
    size_t i;

    if ((src[0] & 1U) != 0U) {
        return NULL;
    }
    if (r == NULL) {
        bridge_remote_t *old = NULL;

        for (i = 0U; i < BRIDGE_REMOTES; ++i) {
            bridge_remote_t *e = &br.remotes[i];

            if (!e->used || old == NULL || e->seen < old->seen) {
                old = e;
                if (!e->used) {
                    break;
                }
            }
        }
        if (old->used && old->neigh_ip_be != 0U) {
            neigh_set(old->neigh_ip_be, old->mac, 0);
        }
        memset(old, 0, sizeof(*old));
        memcpy(old->mac, src, 6);
        old->used = 1U;
        r = old;
    }
    r->seen = time(NULL);
    if (l3 != 0U && type == 0x0800U && len >= l3 + 20U) {
        memcpy(&ip, frame + l3 + 12, 4);
    } else if (l3 != 0U && type == 0x0806U && len >= l3 + 28U) {
        memcpy(&ip, frame + l3 + 14, 4);
    }
    if (ip != 0U && on_lan(ip) && r->neigh_ip_be != ip &&
        (r->refused_ip_be != ip ||
         difftime(r->seen, r->refused_at) >= (double)BRIDGE_CLAIM_RECHECK)) {
        int may = neigh_may_take(ip, r->mac,
                                 r->refused_ip_be == ip && r->probed);

        if (may == 1) {
            r->refused_ip_be = 0U;
            r->probed = 0U;
        } else {
            char a[INET_ADDRSTRLEN];

            if (r->refused_ip_be != ip && may != -2) {   /* once */
                (void)inet_ntop(AF_INET, &ip, a, sizeof(a));
                log_message(LOG_WARNING, "Bridge %s: a tunnel device "
                            "%02x:%02x:%02x:%02x:%02x:%02x claims %s, a LAN "
                            "device's address: ignored", br.ifname,
                            r->mac[0], r->mac[1], r->mac[2], r->mac[3],
                            r->mac[4], r->mac[5], a);
            }
            r->refused_ip_be = ip;
            r->refused_at = r->seen;
            /* Probed only when the request went out (#212) */
            r->probed = may == -1 && claim_probe(ip) == 0 ? 1U : 0U;
        }
    }
    if (ip != 0U && on_lan(ip) && r->refused_ip_be != ip) {
        r->ip_be = ip;
        if (r->neigh_ip_be != ip) {
            if (r->neigh_ip_be != 0U) {
                neigh_set(r->neigh_ip_be, r->mac, 0);
            }
            neigh_set(ip, r->mac, 1);
            r->neigh_ip_be = ip;
        }
    }
    return r;
}

/* ---------------------------------------------------------------- */
/* Sending                                                            */
/* ---------------------------------------------------------------- */

/* Frames camex put on the wire come back on the packet socket as
 * outgoing ones: a short memory of them keeps them from the tunnel (in
 * nat mode their source is the host's MAC, so it cannot tell them) */
#define BRIDGE_ECHOES 64U

static struct {
    uint32_t h;
    time_t at;
} echoes[BRIDGE_ECHOES];
static size_t echo_next = 0U;

static uint32_t frame_hash(const uint8_t *frame, size_t len)
{
    uint32_t h = 2166136261U ^ (uint32_t)len;
    size_t n = len < 96U ? len : 96U;
    size_t i;

    for (i = 0U; i < n; ++i) {
        h = (h ^ frame[i]) * 16777619U;
    }
    return h;
}

static int echo_of_ours(const uint8_t *frame, size_t len)
{
    uint32_t h = frame_hash(frame, len);
    time_t now = time(NULL);
    size_t i;

    for (i = 0U; i < BRIDGE_ECHOES; ++i) {
        if (echoes[i].h == h && difftime(now, echoes[i].at) <= 2.0) {
            echoes[i].at = 0;
            return 1;
        }
    }
    return 0;
}

/* A frame onto the wire */
static int wire_send(const uint8_t *frame, size_t len)
{
    static uint8_t buf[sizeof(vnet_hdr_t) + L2_FRAME_MAX];
    size_t hl = br.vnet ? sizeof(vnet_hdr_t) : 0U;
    struct sockaddr_ll to;

    if (len > L2_FRAME_MAX) {
        return -1;
    }
    echoes[echo_next].h = frame_hash(frame, len);
    echoes[echo_next].at = time(NULL);
    echo_next = (echo_next + 1U) % BRIDGE_ECHOES;
    memset(buf, 0, hl);
    memcpy(buf + hl, frame, len);
    memset(&to, 0, sizeof(to));
    to.sll_family = AF_PACKET;
    to.sll_ifindex = br.ifindex;
    to.sll_halen = 6;
    memcpy(to.sll_addr, frame, 6);
    return sendto(br.pkt, buf, hl + len, MSG_DONTWAIT,
                  (struct sockaddr *)&to, sizeof(to)) < 0 ? -1 : 0;
}

/* An IPv4 packet for the host's own stack */
static int host_deliver(const uint8_t *ip, size_t len)
{
    struct sockaddr_in to;

    if (br.raw < 0 || len < 20U) {
        return -1;
    }
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    memcpy(&to.sin_addr, ip + 16, 4);
    return sendto(br.raw, ip, len, MSG_DONTWAIT, (struct sockaddr *)&to,
                  sizeof(to)) < 0 ? -1 : 0;
}

/* A frame for the tunnel, cut to the MTU when the kernel merged it */
static int tunnel_send(uint8_t *frame, size_t len, const vnet_hdr_t *vh)
{
    uint16_t type;
    size_t l3 = l3_offset(frame, len, &type);

    if (vh != NULL && (vh->gso_type & ~VNET_GSO_ECN) != VNET_GSO_NONE) {
        if ((vh->gso_type & ~VNET_GSO_ECN) == VNET_GSO_TCPV4 ||
            (vh->gso_type & ~VNET_GSO_ECN) == VNET_GSO_TCPV6) {
            return bridge_segment(frame, len, vh->gso_size, br.to_tunnel);
        }
        return -1;   /* UDP fragmentation offload: not taken */
    }
    if (vh != NULL && (vh->flags & VNET_F_NEEDS_CSUM) != 0U && l3 != 0U) {
        (void)bridge_fix_checksum(frame, len, l3);
    }
    if (len > (size_t)br.mtu + L2_ETH_HDR + 4U) {
        if (!br.warned_big) {
            br.warned_big = 1U;
            log_message(LOG_WARNING, "Bridge %s: a frame of %u bytes, more "
                        "than the MTU, is dropped", br.ifname,
                        (unsigned)len);
        }
        return -1;
    }
    return br.to_tunnel(frame, len);
}

/* ---------------------------------------------------------------- */
/* nat mode (a Wi-Fi station)                                        */
/* ---------------------------------------------------------------- */

/* From the tunnel to the LAN: the host's MAC as source, in ARP too;
 * DHCP asks for broadcast answers. 0, or -1 to drop. */
static int nat_out(uint8_t *frame, size_t len)
{
    uint16_t type;
    size_t l3 = l3_offset(frame, len, &type);

    if (l3 == 0U) {
        return -1;
    }
    if (type == 0x0806U && len >= l3 + 28U) {
        memcpy(frame + 6, br.host_mac, 6);
        memcpy(frame + l3 + 8, br.host_mac, 6);   /* sender hardware */
        return 0;
    }
    if (type != 0x0800U || len < l3 + 20U) {
        return -1;   /* nat mode carries IPv4 and ARP only */
    }
    memcpy(frame + 6, br.host_mac, 6);
    {
        uint8_t *ip = frame + l3;
        size_t ihl = (size_t)(ip[0] & 0x0FU) * 4U;

        /* DHCP from a remote client: the answer must come by broadcast,
         * the LAN knows the client only by the host's MAC */
        if (ip[9] == 17U && len >= l3 + ihl + 8U + 12U &&
            ip[ihl + 2U] == 0U && ip[ihl + 3U] == 67U) {
            uint8_t *bootp = ip + ihl + 8U;

            bootp[10] |= 0x80U;   /* flags: broadcast */
            ip[ihl + 6U] = ip[ihl + 7U] = 0U;   /* no UDP checksum */
        }
    }
    return 0;
}

/* From the LAN in nat mode: a frame to the host's MAC whose IPv4
 * destination is a remote device, or an ARP reply for one, goes to that
 * device. Returns the remote's MAC, or NULL. */
static const uint8_t *nat_in(uint8_t *frame, size_t len)
{
    uint16_t type;
    size_t l3 = l3_offset(frame, len, &type);
    uint32_t ip = 0U;
    bridge_remote_t *r;

    if (l3 == 0U) {
        return NULL;
    }
    if (type == 0x0800U && len >= l3 + 20U) {
        memcpy(&ip, frame + l3 + 16, 4);
    } else if (type == 0x0806U && len >= l3 + 28U) {
        memcpy(&ip, frame + l3 + 24, 4);
    }
    r = remote_by_ip(ip);
    if (r == NULL) {
        return NULL;
    }
    memcpy(frame, r->mac, 6);
    if (type == 0x0806U) {
        memcpy(frame + l3 + 18, r->mac, 6);       /* target hardware */
    }
    return r->mac;
}

/* ---------------------------------------------------------------- */
/* Probes                                                             */
/* ---------------------------------------------------------------- */

static void probe_send(void)
{
    uint8_t f[L2_ETH_HDR + 8U + 1U + 64U];
    size_t idl = strlen(br.client_id);

    memcpy(f, mac_bcast, 6);
    memcpy(f + 6, br.host_mac, 6);
    f[12] = (uint8_t)(BRIDGE_ETHERTYPE >> 8);
    f[13] = (uint8_t)BRIDGE_ETHERTYPE;
    memcpy(f + L2_ETH_HDR, br.probe_tag, 8);
    f[L2_ETH_HDR + 8U] = (uint8_t)idl;
    memcpy(f + L2_ETH_HDR + 9U, br.client_id, idl);
    (void)wire_send(f, L2_ETH_HDR + 9U + idl < 60U ? 60U
                                                  : L2_ETH_HDR + 9U + idl);
}

static void probe_seen(const uint8_t *frame, size_t len)
{
    char id[65];
    size_t idl;

    if (len < L2_ETH_HDR + 9U ||
        memcmp(frame + L2_ETH_HDR, br.probe_tag, 8) != 0) {
        return;   /* another network's bridge: not ours to mind */
    }
    idl = frame[L2_ETH_HDR + 8U];
    if (idl == 0U || idl > 64U || len < L2_ETH_HDR + 9U + idl) {
        return;
    }
    memcpy(id, frame + L2_ETH_HDR + 9U, idl);
    id[idl] = '\0';
    if (strcmp(id, br.client_id) >= 0) {
        return;   /* ours, or a higher ID: that one stops */
    }
    if (br.peer_until == 0 || strcmp(br.peer_id, id) != 0) {
        log_message(LOG_WARNING, "Bridge %s: client %s bridges this network "
                    "on the same LAN; bridging stops while it does (a loop "
                    "otherwise)", br.ifname, id);
    }
    snprintf(br.peer_id, sizeof(br.peer_id), "%s", id);
    br.peer_until = time(NULL) + BRIDGE_PEER_HOLD;
}

/* A probe of a higher-ID bridge: it is listening, answer at once */
static void probe_answer(const uint8_t *frame, size_t len)
{
    if (len >= L2_ETH_HDR + 9U &&
        memcmp(frame + L2_ETH_HDR, br.probe_tag, 8) == 0 &&
        br.peer_until == 0) {
        probe_send();
    }
}

static int bridge_paused(void)
{
    /* Listening first: a bridge that started now forwards nothing until
     * a probe interval went by without a lower-ID bridge of the network */
    if (br.listen_until != 0) {
        if (difftime(time(NULL), br.listen_until) < 0.0) {
            return 1;
        }
        br.listen_until = 0;
        if (br.peer_until == 0) {
            log_message(LOG_NOTICE, "Bridge %s: no other bridge of this "
                        "network on the LAN, bridging", br.ifname);
        }
    }
    if (br.peer_until != 0 && difftime(time(NULL), br.peer_until) > 0.0) {
        log_message(LOG_NOTICE, "Bridge %s: client %s is gone from the "
                    "LAN, bridging again", br.ifname, br.peer_id);
        br.peer_until = 0;
    }
    return br.peer_until != 0;
}

/* ---------------------------------------------------------------- */
/* The interface                                                      */
/* ---------------------------------------------------------------- */

static void host_addr_refresh(void)
{
    struct ifreq ifr;
    int s = socket(AF_INET, SOCK_DGRAM, 0);

    if (s < 0) {
        return;
    }
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", br.ifname);
    if (ioctl(s, SIOCGIFADDR, &ifr) == 0) {
        memcpy(&br.host_ip_be,
               &((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr, 4);
    } else {
        br.host_ip_be = 0U;
    }
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", br.ifname);
    if (ioctl(s, SIOCGIFNETMASK, &ifr) == 0) {
        memcpy(&br.host_mask_be,
               &((struct sockaddr_in *)&ifr.ifr_netmask)->sin_addr, 4);
    } else {
        br.host_mask_be = 0U;
    }
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", br.ifname);
    if (ioctl(s, SIOCGIFMTU, &ifr) == 0) {
        br.mtu = ifr.ifr_mtu;
    }
    close(s);
}

static int is_wifi(const char *ifname)
{
    char path[96];
    struct stat st;

    snprintf(path, sizeof(path), "/sys/class/net/%.15s/phy80211", ifname);
    if (stat(path, &st) == 0) {
        return 1;
    }
    snprintf(path, sizeof(path), "/sys/class/net/%.15s/wireless", ifname);
    return stat(path, &st) == 0;
}

int bridge_open(const char *ifname, bridge_mode_t mode,
                const uint8_t net_id[32], const char *client_id,
                bridge_send_fn to_tunnel)
{
    struct ifreq ifr;
    struct sockaddr_ll sll;
    struct packet_mreq mr;
    int one = 1;

    bridge_close();
    memset(&br, 0, sizeof(br));
    br.pkt = br.raw = br.nl = -1;
    br.ready = 1;
    snprintf(br.ifname, sizeof(br.ifname), "%s", ifname);
    snprintf(br.client_id, sizeof(br.client_id), "%s", client_id);
    memcpy(br.probe_tag, net_id, 8);
    br.to_tunnel = to_tunnel;
    br.mtu = 1500;
    br.ifindex = (int)if_nametoindex(ifname);
    if (br.ifindex == 0) {
        log_message(LOG_ERR, "Bridge: no interface %s", ifname);
        return -1;
    }
    br.pkt = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (br.pkt < 0) {
        print_errno_message(LOG_ERR, "socket(AF_PACKET)");
        return -1;
    }
    /* The kernel's offload metadata with every frame (bridge.h) */
    br.vnet = setsockopt(br.pkt, SOL_PACKET, PACKET_VNET_HDR, &one,
                         sizeof(one)) == 0;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex = br.ifindex;
    if (bind(br.pkt, (struct sockaddr *)&sll, sizeof(sll)) != 0) {
        print_errno_message(LOG_ERR, "bind(AF_PACKET)");
        bridge_close();
        return -1;
    }
    /* Promiscuous through the socket: the kernel drops it when the
     * socket closes, the interface's own flags are not touched */
    memset(&mr, 0, sizeof(mr));
    mr.mr_ifindex = br.ifindex;
    mr.mr_type = PACKET_MR_PROMISC;
    if (setsockopt(br.pkt, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr,
                   sizeof(mr)) != 0) {
        print_errno_message(LOG_WARNING, "PACKET_ADD_MEMBERSHIP");
    }
    if (set_fd_nonblocking(br.pkt) != 0) {
        bridge_close();
        return -1;
    }
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    if (ioctl(br.pkt, SIOCGIFHWADDR, &ifr) != 0 ||
        ifr.ifr_hwaddr.sa_family != ARPHRD_ETHER) {
        log_message(LOG_ERR, "Bridge: %s is not an Ethernet interface",
                    ifname);
        bridge_close();
        return -1;
    }
    memcpy(br.host_mac, ifr.ifr_hwaddr.sa_data, 6);
    br.raw = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
    if (br.raw < 0) {
        print_errno_message(LOG_WARNING, "socket(IPPROTO_RAW): remote "
                            "devices cannot reach this host");
    }
    host_addr_refresh();
    br.nat = mode == BRIDGE_NAT || (mode == BRIDGE_AUTO && is_wifi(ifname));
    br.open = 1;
    {
        FILE *fp = fopen("/proc/sys/net/ipv4/ip_forward", "r");
        int fwd = 0;

        if (fp != NULL) {
            if (fscanf(fp, "%d", &fwd) != 1) {
                fwd = 0;
            }
            fclose(fp);
        }
        if (fwd && br.nat) {
            log_message(LOG_WARNING, "Bridge %s: IP forwarding is on: the "
                        "host also routes packets meant for remote devices",
                        ifname);
        }
    }
    {
        char macs[18];

        l2_mac_text(br.host_mac, macs, sizeof(macs));
        log_message(LOG_NOTICE, "Bridge on %s (%s, %s mode%s): remote devices "
                    "are on its LAN", ifname, macs, br.nat ? "nat" : "full",
                    br.vnet ? "" : ", no offload data");
    }
    probe_send();
    br.last_probe = time(NULL);
    br.listen_until = br.last_probe + BRIDGE_PROBE_EVERY + 1;
    return 0;
}

int bridge_active(void)
{
    return br.open;
}

int bridge_fd(void)
{
    return br.open ? br.pkt : -1;
}

void bridge_readable(void)
{
    static uint8_t buf[BRIDGE_FRAME_MAX];
    unsigned int budget = 256U;

    while (br.open && budget-- > 0U) {
        struct sockaddr_ll from;
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(br.pkt, buf, sizeof(buf), MSG_DONTWAIT,
                             (struct sockaddr *)&from, &fl);
        size_t hl = br.vnet ? sizeof(vnet_hdr_t) : 0U;
        vnet_hdr_t vh;
        uint8_t *frame;
        size_t len;
        uint16_t type;
        int outgoing;

        if (n < 0) {
            break;
        }
        if ((size_t)n < hl + L2_ETH_HDR) {
            continue;
        }
        if (hl > 0U) {
            memcpy(&vh, buf, sizeof(vh));
        }
        frame = buf + hl;
        len = (size_t)n - hl;
        outgoing = from.sll_pkttype == PACKET_OUTGOING;
        (void)l3_offset(frame, len, &type);
        if (type == BRIDGE_ETHERTYPE) {
            if (!outgoing) {
                probe_seen(frame, len);
                probe_answer(frame, len);
            }
            continue;
        }
        /* What came from the tunnel (sent onto the wire by camex, or
         * flooded back by a switch) does not go back */
        if (remote_find(frame + 6) != NULL ||
            (outgoing && echo_of_ours(frame, len))) {
            continue;
        }
        if (bridge_paused()) {
            continue;
        }
        if ((frame[0] & 1U) != 0U) {
            /* Broadcast, multicast: the host's own and the LAN's */
            (void)tunnel_send(frame, len, hl > 0U ? &vh : NULL);
            continue;
        }
        if (remote_find(frame) != NULL) {
            (void)tunnel_send(frame, len, hl > 0U ? &vh : NULL);
            continue;
        }
        if (br.nat && !outgoing && memcmp(frame, br.host_mac, 6) == 0 &&
            nat_in(frame, len) != NULL) {
            (void)tunnel_send(frame, len, hl > 0U ? &vh : NULL);
        }
    }
}

int bridge_from_tunnel(const uint8_t *in, size_t len)
{
    static uint8_t frame[L2_FRAME_MAX];
    uint16_t type;
    size_t l3;

    if (!br.open || len < L2_ETH_HDR || len > sizeof(frame)) {
        return -1;
    }
    memcpy(frame, in, len);
    l3 = l3_offset(frame, len, &type);
    if (type == BRIDGE_ETHERTYPE) {
        return 0;
    }
    (void)remote_learn(frame, len);
    if (bridge_paused()) {
        return 0;
    }
    /* To the host itself */
    if (memcmp(frame, br.host_mac, 6) == 0) {
        if (type == 0x0800U && l3 != 0U) {
            uint32_t dst;

            memcpy(&dst, frame + l3 + 16, 4);
            if (dst == br.host_ip_be) {
                return host_deliver(frame + l3, len - l3);
            }
            if (br.nat) {
                /* A remote device to a LAN host it knows by our MAC */
                (void)nat_out(frame, len);
                return 0;
            }
        }
        return 0;   /* an ARP reply to the host: the neighbour entry did it */
    }
    /* ARP for the host's address: camex answers for it, the host never
     * sees what is sent onto the wire */
    if (type == 0x0806U && br.host_ip_be != 0U) {
        uint8_t reply[L2_ETH_HDR + 28U];
        size_t rl = l2_arp_reply(frame, len, br.host_ip_be, br.host_mac,
                                 reply, sizeof(reply));

        if (rl > 0U) {
            return br.to_tunnel(reply, rl);
        }
    }
    if (br.nat && nat_out(frame, len) != 0) {
        return 0;
    }
    return wire_send(frame, len);
}

void bridge_tick(void)
{
    time_t now = time(NULL);

    if (!br.open) {
        return;
    }
    (void)bridge_paused();   /* the end of listening, a peer gone */
    if (difftime(now, br.last_probe) >= (double)BRIDGE_PROBE_EVERY) {
        br.last_probe = now;
        probe_send();
    }
    if (difftime(now, br.last_host_check) >= 10.0) {
        br.last_host_check = now;
        host_addr_refresh();
    }
}

void bridge_close(void)
{
    size_t i;

    if (!br.ready) {
        return;   /* never opened: its zeros are no sockets */
    }
    for (i = 0U; i < BRIDGE_REMOTES; ++i) {
        if (br.remotes[i].used && br.remotes[i].neigh_ip_be != 0U &&
            br.ifindex != 0) {
            neigh_set(br.remotes[i].neigh_ip_be, br.remotes[i].mac, 0);
        }
        br.remotes[i].used = 0U;
    }
    if (br.pkt >= 0) {
        close(br.pkt);
    }
    if (br.raw >= 0) {
        close(br.raw);
    }
    if (br.nl >= 0) {
        close(br.nl);
    }
    br.pkt = br.raw = br.nl = -1;
    br.open = 0;
}

#else   /* not Linux */

int bridge_open(const char *ifname, bridge_mode_t mode,
                const uint8_t net_id[32], const char *client_id,
                bridge_send_fn to_tunnel)
{
    (void)ifname;
    (void)mode;
    (void)net_id;
    (void)client_id;
    (void)to_tunnel;
    log_message(LOG_ERR, "--bridge needs Linux");
    return -1;
}

int bridge_active(void)
{
    return 0;
}

int bridge_fd(void)
{
    return -1;
}

void bridge_readable(void)
{
}

int bridge_from_tunnel(const uint8_t *frame, size_t len)
{
    (void)frame;
    (void)len;
    return -1;
}

void bridge_tick(void)
{
}

void bridge_close(void)
{
}

int bridge_fix_checksum(uint8_t *frame, size_t len, size_t l3)
{
    (void)frame;
    (void)len;
    (void)l3;
    return -1;
}

int bridge_segment(const uint8_t *frame, size_t len, size_t mss,
                   int (*send)(const uint8_t *frame, size_t len))
{
    (void)frame;
    (void)len;
    (void)mss;
    (void)send;
    return -1;
}
#endif
