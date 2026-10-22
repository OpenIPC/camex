/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * server.h — server side of protocol v2: client table, handshakes,
 *            sessions, packet forwarding
 *
 */

#ifndef CAMEX_SERVER_H
#define CAMEX_SERVER_H

#include "camex.h"
#include "net.h"
#include "session.h"

#include <stdint.h>
#include <stddef.h>

#ifndef _WIN32
#include <netinet/in.h>
#else
#include <winsock2.h>
#endif

/* Handshakes (INIT messages) processed per second, all clients together:
 * each costs X25519 operations */
#define SERVER_HANDSHAKES_PER_SEC 200U

/* Seconds after the start in which the auto pool gives clients only the
 * address they ask for, their previous one (#81): returning clients come
 * back first, so no address is handed out twice */
#define SERVER_POOL_HOLD 60

/* Keys tried per second, all handshakes together (#75): each handshake
 * tries the keystore in turn (the global key first, not counted; a live
 * client's own key before the others, #78) until one opens it. The bound
 * is the old worst case, 200 handshakes with 256 keys each, or four times
 * the keystore when it is larger, so that every key can be reached. Both
 * budgets are shared out by source (#78, server_handshake_budget()). */
#define SERVER_KEY_TRIALS_PER_SEC (SERVER_HANDSHAKES_PER_SEC * 256U)

/* Per-server-client state */
typedef struct {
    struct sockaddr_in addr;
    uint32_t ip_be;       /* tunnel address of the current session */
    uint32_t pending_ip_be;  /* new address sent in a CONFIG, taken when
                              * the client confirms the new session
                              * (#77); reserved meanwhile; 0 = none */
    uint32_t prev_ip_be;  /* address of the previous session (#77) */
    time_t last_seen;
    time_t last_data;     /* last authenticated DATA (not a handshake) */
    char client_id[CAMEX_CLIENT_ID_LEN];
    uint8_t active;
    uint8_t registered;   /* a handshake was accepted for this entry; until
                           * then a TCP slot is pre-auth (#9) */
    uint8_t auto_client;  /* registered in auto mode (#15) */
    time_t connected_at;  /* TCP accept time: the pre-auth deadline counts
                           * from here, not from last_seen (#9) */
    int tcp_fd;           /* TCP socket fd for this client (-1 = UDP) */
    uint8_t psk_key[32];  /* key of the last handshake (for reloads) */
    uint8_t hs_ts[V2_TIMESTAMP_LEN];  /* its timestamp: a live session
                                       * takes only newer ones */
    time_t registered_at; /* its first accepted handshake (#82) */
    time_t session_since; /* its current connection: the first handshake
                           * after a (re)connect; 0 until then */
    time_t last_hs;       /* its latest accepted handshake */
    uint32_t handshakes;  /* accepted handshakes */
    uint64_t rx_packets;  /* authenticated packets from it, keepalives too */
    uint64_t rx_bytes;    /* their tunnel payload */
    uint64_t tx_packets;  /* tunnel packets sent to it */
    uint64_t tx_bytes;
    time_t hs_window;     /* INITs from its address this second, at most */
    uint8_t hs_count;     /* SERVER_KNOWN_PER_SEC as a known source (#78) */
    session_t cur;        /* current session: the server sends on it */
    session_t next;       /* from the latest handshake: receives only, and
                           * becomes cur with the client's first packet on
                           * it (so a rekey loses nothing in flight) */
    session_t prev;       /* previous session, receives until prev_until */
    time_t prev_until;
    tcp_recv_state_t recv_state;  /* per-connection partial-read tracker —
                                    * zero on accept/reuse, see net.h */
} server_client_t;

/* Live client slots, allocated at startup (#75) */
extern server_client_t *server_clients;
extern size_t server_max_clients;

/* Allocate count empty slots (replacing any table); 0 or -1 */
int server_clients_init(size_t count);

/* Wipe and free the table */
void server_clients_free(void);

/* Slots when max_clients= is not given: the auto pool's size, at least
 * CAMEX_MAX_CLIENTS_DEFAULT, at most CAMEX_MAX_CLIENTS_LIMIT */
size_t server_default_max_clients(void);

/* Warn when the auto pool does not fit the server's tunnel address
 * (local_cidr); nothing without one */
void server_check_pool(const char *server_cidr);

/* Set a key of an entry and index it: always through these, or the
 * lookups below do not find the entry */
void server_set_ip(server_client_t *entry, uint32_t ip_be);
void server_set_id(server_client_t *entry, const char *client_id);
void server_set_addr(server_client_t *entry, const struct sockaddr_in *addr);

/* Find clients */
server_client_t *server_find_by_ip(uint32_t ip_be);
server_client_t *server_find_by_client_id(const char *client_id);
server_client_t *server_find_by_addr(const struct sockaddr_in *addr);
server_client_t *server_alloc_client(void);

/* Close the entry's TCP connection (if any), wipe its keys, free it */
/* End an entry whose client may still be alive: an auto client's address
 * stays leased to its ID (#81). server_drop_entry() keeps no lease. */
void server_end_client(server_client_t *entry);
void server_drop_entry(server_client_t *entry);

/* Expire stale clients and old sessions */
void server_expire_clients(void);

/* After a config reload: end sessions whose key is no longer allowed
 * for their client (#16 L3). */
void server_revalidate_keys(void);

/* After a config reload: end sessions of auto clients whose ID is not in
 * the file when allow_unknown_clients = no (#15). */
void server_revalidate_unknown_clients(void);

/* After a reload: end sessions of clients whose address is now another
 * client's [client ID] local_cidr (#81) */
void server_revalidate_profile_addresses(void);

/* Forward an IP packet from one client to another (or local TUN) */
int server_forward_packet(const uint8_t *packet, size_t len,
                          uint32_t src_ip_be, uint32_t dst_ip_be);

/* Main server packet handler.
 * fd: current TCP connection's fd for this packet, or -1 for UDP. */
int server_handle_packet(const uint8_t *buffer, size_t len,
                         const struct sockaddr_in *from, int fd);

/* Server socket creation */
int server_socket_create(const char *bind_ip, int port,
                         const char *bind_dev);

/* The status of the server and its clients for camex --status (#82): a
 * table (json 0) or one JSON object (json 1), malloc'd and NUL-terminated,
 * its length in *len; NULL without memory */
char *server_status(int json, size_t *len);

#endif /* CAMEX_SERVER_H */
