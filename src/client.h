/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * client.h — client-side state machine (register, reconnect, packet handling)
 *
 */

#ifndef CAMEX_CLIENT_H
#define CAMEX_CLIENT_H

#include "camex.h"

#include <stdint.h>
#include <stddef.h>

/* Client state structure */
typedef struct {
    uint64_t send_seq;
    uint64_t recv_seq_max;
    uint64_t recv_window;
    time_t last_register;
    time_t last_recv;
    time_t last_keepalive;
    uint8_t registered;
    uint8_t config_received;
    uint8_t send_nonce_prefix[4];
} client_state_t;

extern client_state_t client_state;
extern time_t client_reconnect_at;
extern uint8_t client_link_up;

/* Initialize client state */
void client_state_init(client_state_t *state);

/* Send REGISTER to server */
int client_send_register(void);

/* Handle an incoming UDP/TCP packet (client-side) */
int client_handle_net_packet(const uint8_t *buffer, size_t len);

/* Handle a TUN packet (encrypt and send to server) */
int client_handle_tun_packet(const uint8_t *buffer, size_t len);

/* Apply a config response (plaintext payload) */
int client_apply_config_response(const uint8_t *payload, size_t payload_len);

/* Wait for auto-config response from server */
int client_wait_for_config(void);

/* Reconnect (re-resolve, recreate socket, re-register) */
void client_reconnect(void);

/* Periodic client tick (reconnect, re-register, silence detection) */
void client_tick(void);

/* Client socket creation */
int client_socket_create(const char *host, int port,
                         const char *bind_dev);

#endif /* CAMEX_CLIENT_H */
