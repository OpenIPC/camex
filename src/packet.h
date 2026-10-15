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
    ssize_t (*read_packet)(uint8_t *buffer, size_t size);
    int (*write_packet)(const uint8_t *buffer, size_t len);
    intptr_t (*get_fd)(void);  /* descriptor for select(), -1 if none */
    void (*close)(void);
} camex_packet_backend_t;

/* TUN devices: Linux /dev/net/tun or /dev/camex, macOS utun, WinTUN,
 * Android (provider or CAMEX_TUN_FD) */
extern const camex_packet_backend_t camex_tun_backend;

/* Select the backend; NULL restores the TUN backend (the default) */
void packet_backend_set(const camex_packet_backend_t *backend);
const camex_packet_backend_t *packet_backend_get(void);

int packet_open(const camex_packet_open_t *params);
ssize_t packet_read(uint8_t *buffer, size_t size);
int packet_write(const uint8_t *buffer, size_t len);
intptr_t packet_fd(void);
void packet_close(void);

#endif /* CAMEX_PACKET_H */
