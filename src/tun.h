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
#include <sys/types.h>

/*
 * TUN file descriptor (global, accessible from modules).
 * On POSIX: native fd (int).
 * On Windows: Winsock SOCKET stored as int (signal socket for WinTUN).
 * Windows socket handles are typically small integers; the cast is safe.
 * For Win64, SOCKET is 64-bit, so we use intptr_t for portability.
 */
#include <stdint.h>
#ifndef _WIN32
extern int tun_fd;
#else
extern intptr_t tun_fd;
#endif
extern char tun_name[];

/* More verbose crypto error logging */
/* Maximum TUN packet size (can be overridden via -DTUN_PACKET_MAX) */
#ifndef TUN_PACKET_MAX
#define TUN_PACKET_MAX 9216U
#endif

/* Create TUN device */
int tun_create_device(const char *local_ip, const char *netmask, int mtu,
                      const char *tun_dev_override);

/* Accept TUN fd from environment (CAMEX_TUN_FD) — used by Android VpnService */
int tun_accept_fd(void);

/*
 * Optional TUN provider: lets the host application create the TUN device
 * itself once the tunnel parameters are known (Android: VpnService.Builder
 * needs the address, MTU and routes from the server's CONFIG). Returns a
 * TUN fd that camex then owns, or -1.
 */
typedef int (*camex_tun_provider_t)(const char *local_ip, int prefix_len,
                                    int mtu, const char routes[][32],
                                    uint8_t route_count, void *ctx);
void tun_set_provider(camex_tun_provider_t provider, void *ctx);

/* Open the TUN device through the provider; -1 if none is set or it fails */
int tun_open_provided(const char *local_ip, const char *netmask, int mtu,
                      const char routes[][32], uint8_t route_count);

/* 1 if the TUN device came from outside (provider or CAMEX_TUN_FD): routes
 * are then managed by the host application, not by camex */
int tun_is_external(void);

/* Close TUN device */
void tun_close_device(void);

/* Read a packet from TUN */
ssize_t tun_read_packet(uint8_t *buffer, size_t size);

/* Write a packet to TUN */
int tun_write_packet(const uint8_t *buffer, size_t len);

#endif /* CAMEX_TUN_H */
