/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * client.h — client side of protocol v2: handshake, sessions, reconnect
 *
 */

#ifndef CAMEX_CLIENT_H
#define CAMEX_CLIENT_H

#include "camex.h"
#include "net.h"
#include "noise.h"
#include "session.h"

#include <stdint.h>
#include <stddef.h>

/* Client state */
typedef struct {
    noise_hs_t hs;            /* handshake in progress */
    uint8_t hs_pending;
    uint32_t hs_index;        /* our index for the session it creates */
    time_t hs_started;        /* first INIT of this handshake */
    time_t hs_sent;           /* last INIT sent */
    session_t cur;            /* current session: sends and receives */
    session_t prev;           /* previous session: receives until prev_until */
    time_t prev_until;
    time_t last_recv;         /* last authenticated packet from the server */
    time_t last_keepalive;    /* last keepalive sent */
    uint8_t config_received;  /* auto mode: CONFIG applied */
    uint8_t established;      /* a session has been set up since connect */
    uint8_t confirm_pending;  /* the server has not answered on cur yet:
                               * keepalives repeat until it does */
} client_state_t;

extern client_state_t client_state;
extern time_t client_reconnect_at;
extern uint8_t client_link_up;
/* Persistent partial-read tracker for the client's single TCP connection —
 * must be reset (zeroed) on every reconnect, see tcp_recv_state_t in net.h. */
extern tcp_recv_state_t client_recv_state;

/* Initialize client state */
void client_state_init(client_state_t *state);

/* Start a handshake (send INIT) */
int client_start_handshake(void);

/* Handle an incoming UDP/TCP packet (client-side) */
int client_handle_net_packet(const uint8_t *buffer, size_t len);

/* Handle a TUN packet (encrypt and send to server) */
int client_handle_tun_packet(const uint8_t *buffer, size_t len);

/* Apply a config response (control message) */
int client_apply_config_response(const uint8_t *payload, size_t payload_len);

/* Auto mode: handshake until the server's CONFIG has been applied */
int client_wait_for_config(void);

/* Reconnect (re-resolve, recreate socket, new handshake) */
void client_reconnect(void);

/* Periodic client tick: reconnect, handshake retries, rekey, keepalive,
 * silence detection */
void client_tick(void);

/* Client socket creation */
int client_socket_create(const char *host, int port,
                         const char *bind_dev);

/* Wipe sessions and handshake state */
void client_wipe(void);

#endif /* CAMEX_CLIENT_H */
