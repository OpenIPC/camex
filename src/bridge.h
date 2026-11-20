/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * bridge.h — a client of a TAP network bridges it to a host interface
 *            (--bridge eth0): the remote devices are on that LAN, the
 *            host keeps its settings and still talks to them. Linux,
 *            built with CAMEX_L2.
 *
 * No bridge device and no TAP device: a packet socket on the interface
 * sees every frame on it, the host's own outgoing ones too, and sends
 * frames onto it.
 *
 *  - LAN -> tunnel: broadcast and multicast frames, and frames to a MAC
 *    that came from the tunnel (a remote device)
 *  - tunnel -> LAN: onto the wire, as they are
 *  - host -> remote: the host's frames to a remote MAC, seen as outgoing
 *  - remote -> host: a frame sent on the packet socket never reaches the
 *    host's own stack, so IPv4 packets to the host are handed to it
 *    through a raw IP socket. The host learns the remote devices' MACs as
 *    permanent neighbour entries on the interface (netlink), removed when
 *    camex stops, and camex answers ARP requests for the host's address.
 *  - offloads: with PACKET_VNET_HDR the kernel says which frames still
 *    need their checksum (checksum offload) and which are TCP segments
 *    merged by TSO or GRO; camex completes the checksums and cuts those
 *    into frames that fit the MTU. The interface's offloads stay as they
 *    are.
 *  - loops: every bridge sends a probe frame (EtherType 0x88B5) on its
 *    LAN every few seconds; a bridge that sees another bridge of the same
 *    network with a lower client ID there stops bridging until the other
 *    one is gone.
 *
 * On a Wi-Fi client interface (station) an access point takes frames only
 * from the station's own MAC: there the bridge works in nat mode, like
 * OpenWrt's relayd. Frames to the LAN go out with the host's MAC; ARP is
 * rewritten and answered for the remote devices; a frame to the host's MAC
 * whose IPv4 destination is a remote device goes to that device; DHCP
 * requests keep their client hardware address and ask for broadcast
 * answers. Only IPv4 (and ARP) passes in nat mode.
 *
 */

#ifndef CAMEX_BRIDGE_H
#define CAMEX_BRIDGE_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    BRIDGE_AUTO = 0,   /* nat on a Wi-Fi station, full elsewhere */
    BRIDGE_FULL = 1,   /* every frame as it is */
    BRIDGE_NAT  = 2    /* the host's MAC towards the LAN */
} bridge_mode_t;

#define BRIDGE_ETHERTYPE 0x88B5U   /* the probes (local experimental) */

/* Frames for the tunnel go through this */
typedef int (*bridge_send_fn)(const uint8_t *frame, size_t len);

/* Open the bridge on ifname for the network with this ID (its probes),
 * as client_id. 0, or -1 with a logged reason. */
int bridge_open(const char *ifname, bridge_mode_t mode,
                const uint8_t net_id[32], const char *client_id,
                bridge_send_fn to_tunnel);

/* 1 while a bridge is open */
int bridge_active(void);

/* The packet socket, for select() (-1 when closed) */
int bridge_fd(void);

/* The packet socket is readable: frames for the tunnel go out */
void bridge_readable(void);

/* A frame from the tunnel */
int bridge_from_tunnel(const uint8_t *frame, size_t len);

/* Every second: probes, the host's address */
void bridge_tick(void);

/* Close; the neighbour entries camex made go */
void bridge_close(void);

/* Tests: the TCP/UDP checksum of an IPv4 or IPv6 packet in a frame from
 * offset l3 (0 or -1), and the cutting of a TCP segment of up to 64 KiB
 * into frames of mss bytes of payload (each to send, 0 or -1) */
int bridge_fix_checksum(uint8_t *frame, size_t len, size_t l3);
int bridge_segment(const uint8_t *frame, size_t len, size_t mss,
                   int (*send)(const uint8_t *frame, size_t len));

#endif /* CAMEX_BRIDGE_H */
