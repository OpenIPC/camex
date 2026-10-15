/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * packet.h — packet backend: where tunnel packets are read and written
 *
 */

#ifndef CAMEX_PACKET_H
#define CAMEX_PACKET_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

/* Parameters for opening a backend: the tunnel address and routes */
typedef struct {
    const char *local_ip;
    const char *netmask;
    int mtu;
    const char (*routes)[32];
    uint8_t route_count;
    const char *device;       /* device path override, or "" */
    uint8_t use_provider;     /* try the host application's TUN provider */
} camex_packet_open_t;

/*
 * A packet backend (#20). The client and server read and write tunnel
 * packets only through it, so a source other than a TUN device (e.g. a
 * userspace network stack, #21, or memory in tests) can be plugged in.
 */
typedef struct {
    const char *name;
    int (*open)(const camex_packet_open_t *params);
    /* Non-blocking. Returns the packet length; 0 when no packet is
     * queued; -1 on an error (-1 with errno EAGAIN also means "no packet",
     * as for a non-blocking fd). The event loop reads several packets per
     * readable event and stops at the first result <= 0. */
    ssize_t (*read_packet)(uint8_t *buffer, size_t size);
    int (*write_packet)(const uint8_t *buffer, size_t len);
    /* Descriptor the event loop watches with select(): readable when a
     * packet can be read. Required while the backend is open (-1 only
     * when closed), since packets are read only when it is readable;
     * below FD_SETSIZE on POSIX (checked by packet_open()). */
    intptr_t (*get_fd)(void);
    void (*close)(void);
    /* 1 if camex itself must add and remove the host routes for the
     * tunnel; 0 when the routes are someone else's (Android VpnService) or
     * there is no host interface at all */
    int (*host_routes)(void);
} camex_packet_backend_t;

/* TUN devices: Linux /dev/net/tun or /dev/camex, macOS utun, WinTUN,
 * Android (provider or CAMEX_TUN_FD) */
extern const camex_packet_backend_t camex_tun_backend;

/* Select the backend; NULL restores the TUN backend (the default).
 * Refused (-1) while a backend is open: close it first. */
int packet_backend_set(const camex_packet_backend_t *backend);
const camex_packet_backend_t *packet_backend_get(void);

int packet_open(const camex_packet_open_t *params);
ssize_t packet_read(uint8_t *buffer, size_t size);
int packet_write(const uint8_t *buffer, size_t len);
intptr_t packet_fd(void);
void packet_close(void);
int packet_host_routes(void);

#endif /* CAMEX_PACKET_H */
