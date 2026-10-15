/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * packet.c — packet backend dispatch and the TUN backend
 *
 */

#include "packet.h"
#include "tun.h"

static int tun_backend_open(const camex_packet_open_t *p)
{
    /* TUN from the host application (Android VpnService): through the
     * provider, which gets the final address and routes, or as a ready fd
     * in CAMEX_TUN_FD; otherwise create the device here. */
    if (p->use_provider &&
        tun_open_provided(p->local_ip, p->netmask, p->mtu, p->routes,
                          p->route_count) >= 0) {
        return 0;
    }
    if (tun_accept_fd() >= 0) {
        return 0;
    }
    return tun_create_device(p->local_ip, p->netmask, p->mtu, p->device);
}

static intptr_t tun_backend_fd(void)
{
    return (intptr_t)tun_fd;
}

const camex_packet_backend_t camex_tun_backend = {
    "tun",
    tun_backend_open,
    tun_read_packet,
    tun_write_packet,
    tun_backend_fd,
    tun_close_device
};

static const camex_packet_backend_t *backend = &camex_tun_backend;

void packet_backend_set(const camex_packet_backend_t *b)
{
    backend = (b != NULL) ? b : &camex_tun_backend;
}

const camex_packet_backend_t *packet_backend_get(void)
{
    return backend;
}

int packet_open(const camex_packet_open_t *params)
{
    return backend->open(params);
}

ssize_t packet_read(uint8_t *buffer, size_t size)
{
    return backend->read_packet(buffer, size);
}

int packet_write(const uint8_t *buffer, size_t len)
{
    return backend->write_packet(buffer, len);
}

intptr_t packet_fd(void)
{
    return backend->get_fd();
}

void packet_close(void)
{
    backend->close();
}
