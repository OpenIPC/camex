/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * tun.h — TUN device creation and I/O
 *
 */

#ifndef CAMEX_TUN_H
#define CAMEX_TUN_H

#include <stdint.h>
#include <stddef.h>

/* TUN file descriptor (global, accessible from modules) */
extern int tun_fd;
extern char tun_name[];

/* More verbose crypto error logging */
/* Maximum TUN packet size (can be overridden via -DTUN_PACKET_MAX) */
#ifndef TUN_PACKET_MAX
#define TUN_PACKET_MAX 9216U
#endif

/* Create TUN device */
int tun_create_device(const char *local_ip, const char *netmask, int mtu,
                      const char *tun_dev_override);

/* Close TUN device */
void tun_close_device(void);

/* Read a packet from TUN */
int tun_read_packet(uint8_t *buffer, size_t size);

/* Write a packet to TUN */
int tun_write_packet(const uint8_t *buffer, size_t len);

#endif /* CAMEX_TUN_H */
