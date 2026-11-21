/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * offload.h — TUN offloads (Linux, IFF_VNET_HDR): the kernel hands camex
 *             TCP data of up to 64 KB at once (TSO) and checksums still to
 *             compute; camex cuts it into packets of the tunnel's MTU, and
 *             joins the TCP packets of one connection that arrive together
 *             into one write (as the kernel's GRO does). The kernel then
 *             handles one large packet instead of dozens of small ones.
 *             Nothing changes on the wire.
 *
 */

#ifndef CAMEX_OFFLOAD_H
#define CAMEX_OFFLOAD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* struct virtio_net_hdr (10 bytes), in host byte order as the tun device
 * uses it */
typedef struct {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} offload_vnet_t;

#define OFFLOAD_VNET_LEN       10U
#define OFFLOAD_F_NEEDS_CSUM   1U
#define OFFLOAD_GSO_NONE       0U
#define OFFLOAD_GSO_TCPV4      1U
#define OFFLOAD_GSO_TCPV6      4U
#define OFFLOAD_GSO_ECN        0x80U

/* The largest packet either way: 64 KB of IP */
#define OFFLOAD_MAX            65535U
/* Joined packets: at most 32 KB (~23 packets of 1400 bytes; a camera's
 * receiver joins about 15), half the kernel's 64 KB to keep camex's locked
 * memory small */
#define OFFLOAD_GRO_MAX        32768U

void offload_vnet_parse(offload_vnet_t *vh, const uint8_t in[OFFLOAD_VNET_LEN]);
void offload_vnet_write(uint8_t out[OFFLOAD_VNET_LEN], const offload_vnet_t *vh);

/* A checksum left to the device (NEEDS_CSUM): the sum of pkt from start,
 * the pseudo-header part already in the field at start + off */
int offload_complete_csum(uint8_t *pkt, size_t len, size_t start, size_t off);

/* A TSO packet (IPv4 or IPv6 TCP) cut into packets of mss bytes of data,
 * one at a time; each with its own lengths, sequence number, IPv4 ID and
 * checksums */
typedef struct {
    const uint8_t *pkt;
    size_t len;
    size_t hdrs;        /* IP and TCP headers */
    size_t l4;          /* where TCP starts */
    size_t mss;
    size_t off;         /* data cut so far */
    uint32_t seq;
    uint16_t id;
    int v4;
    int active;
} offload_seg_t;

int offload_seg_init(offload_seg_t *s, const uint8_t *pkt, size_t len,
                     size_t mss);
/* The next packet into out: its length, 0 when there are no more, -1 when
 * out is too small */
ssize_t offload_seg_next(offload_seg_t *s, uint8_t *out, size_t size);

/* TCP packets of one connection joined into one (GRO) */
typedef struct {
    uint8_t head[OFFLOAD_VNET_LEN];   /* room for the vnet header: buf
                                         follows it, so header and packet
                                         can go in one write() */
    uint8_t buf[OFFLOAD_GRO_MAX];
    size_t len;         /* 0: empty */
    size_t hdrs;
    size_t l4;
    size_t mss;         /* the data of each packet; the last may be less */
    size_t count;
    uint32_t next_seq;
    int v4;
    int closed;         /* nothing more may join (a short or PSH packet) */
} offload_gro_t;

/* 1: pkt joined (or started) the batch; 0: it may start a batch, but not
 * join this one (flush, then add it again); -1: not a packet to join (an
 * ACK alone, SYN, FIN, other protocols, IP options and the like) */
int offload_gro_add(offload_gro_t *g, const uint8_t *pkt, size_t len);
/* 1 when pkt is of the batch's connection (ordering: flush the batch
 * before writing it) */
int offload_gro_same_flow(const offload_gro_t *g, const uint8_t *pkt,
                          size_t len);
/* The batch as one packet and its vnet header (no GSO for a single
 * packet); returns its length and empties the batch; 0 when empty */
size_t offload_gro_take(offload_gro_t *g, offload_vnet_t *vh,
                        const uint8_t **pkt);

#endif /* CAMEX_OFFLOAD_H */
