/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * server.h — server-side client tracking, packet forwarding, keystore lookup
 *
 */

#ifndef CAMEX_SERVER_H
#define CAMEX_SERVER_H

#include "camex.h"

#include <stdint.h>
#include <stddef.h>
#include <netinet/in.h>

/* Per-server-client state */
typedef struct {
    struct sockaddr_in addr;
    uint32_t ip_be;
    time_t last_seen;
    time_t last_register_time;
    uint64_t recv_seq_max;
    uint64_t recv_window;
    uint64_t send_seq;
    char client_id[CAMEX_CLIENT_ID_LEN];
    uint8_t active;
    uint8_t send_nonce_prefix[4];
    uint8_t psk_key[32];
} server_client_t;

extern server_client_t server_clients[];

/* Find / upsert client */
server_client_t *server_find_by_ip(uint32_t ip_be);
server_client_t *server_find_by_client_id(const char *client_id);
server_client_t *server_find_by_addr(const struct sockaddr_in *addr);
server_client_t *server_alloc_client(void);
server_client_t *server_upsert_client(uint32_t ip_be,
                                      const struct sockaddr_in *addr);

/* Expire stale clients */
void server_expire_clients(void);

/* Reset replay protection for a client entry */
void server_reset_replay(server_client_t *entry);

/* Send config response */
int server_send_config_response(const struct sockaddr_in *from,
                                server_client_t *entry,
                                const char *client_id);

/* Handle plaintext register */
int server_handle_plain_register(const uint8_t *buffer, size_t len,
                                 const struct sockaddr_in *from,
                                 const uint8_t *used_key);

/* Try all keystore entries to decrypt a packet */
const uint8_t *server_try_all_keys(const uint8_t *buffer, size_t len,
                                   uint8_t *type,
                                   uint8_t *plain, size_t plain_size,
                                   size_t *plain_len, uint64_t *seq);

/* Forward an IP packet from one client to another (or local TUN) */
int server_forward_packet(const uint8_t *packet, size_t len,
                          uint32_t src_ip_be, uint32_t dst_ip_be);

/* Main server packet handler */
int server_handle_packet(const uint8_t *buffer, size_t len,
                         const struct sockaddr_in *from);

/* Server socket creation */
int server_socket_create(const char *bind_ip, int port,
                         const char *bind_dev);

#endif /* CAMEX_SERVER_H */
