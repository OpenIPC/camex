/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * netw.h — named networks: a client joins with the server's address, the
 *          network's name and its ID; its address comes from the name and
 *          the ID, the same after any restart
 *
 * A network is known by its ID, BLAKE2b-256 of its name: the client sends
 * the ID (never the name) inside Noise message 1, which only the server's
 * private key opens. The server lists networks in [network NAME] blocks,
 * or, with allow_new_networks=yes, takes any ID as a new network.
 *
 * Each network has an IPv4 pool: pool= of its block, else a /16 (by
 * default) of the network supernet chosen by the ID. The pool's first
 * address is the gateway (the server's address in the network); a client
 * gets its own address of the pool: the one a hash of the network ID and
 * its client ID picks, or, when another client ID holds that one, the
 * nearest free one (+1, -1, +2, -2, ...).
 *
 */

#ifndef CAMEX_NETW_H
#define CAMEX_NETW_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

#define CAMEX_NET_ID_LEN     32U
#define CAMEX_NET_NAME_MAX   63U    /* bytes of a network name */
#define CAMEX_NET_LABEL_MAX  31U    /* label= */
#define CAMEX_NET_MAX        256U   /* networks of a server, all kinds */
#define CAMEX_NET_PROBES     64U    /* addresses tried around the hash */
#define CAMEX_NET_ID_B64_LEN 44U    /* the ID in base64, in REGISTER */

/* Default network supernet and pool prefix: 10.0.0.0/8, /16 pools */
#define CAMEX_NET_SUPERNET_DEFAULT_NET    0x0A000000U
#define CAMEX_NET_SUPERNET_DEFAULT_PREFIX 8U
#define CAMEX_NET_POOL_PREFIX_DEFAULT     16U

/* A [network NAME] block of the server's file */
typedef struct {
    uint8_t id[CAMEX_NET_ID_LEN];
    char label[CAMEX_NET_LABEL_MAX + 1U];   /* label=, "" = none */
    uint32_t pool_net;      /* pool=, host order */
    uint8_t pool_prefix;
    uint8_t pool_set;
    int8_t isolation;       /* client_isolation=: -1 not set, 0, 1 */
    uint8_t tap;            /* mode = tap: an Ethernet network (CAMEX_L2) */
    uint8_t noaddr;         /* addresses = none: camex gives no addresses */
    uint8_t mesh;           /* mesh = yes: peers go direct (CAMEX_MESH) */
} camex_net_conf_t;

/* A network of the running server */
typedef struct {
    uint8_t id[CAMEX_NET_ID_LEN];
    char label[CAMEX_NET_LABEL_MAX + 1U];
    uint32_t pool_net;      /* host order */
    uint8_t pool_prefix;
    uint8_t active;
    uint8_t listed;         /* from a [network NAME] block */
    uint8_t pool_set;       /* its pool came from pool= */
    int8_t isolation;       /* -1: the server's client_isolation */
    uint8_t tap;            /* an Ethernet network */
    uint8_t noaddr;         /* camex gives its clients no addresses */
    uint8_t mesh;           /* clients go peer to peer where they can */
    uint8_t alias_up;       /* the server put alias_ip on its tunnel */
    uint32_t alias_ip;      /* that address, host order */
    time_t created;
} camex_network_t;

extern camex_network_t netw_table[CAMEX_NET_MAX];

/* ID of a network name: BLAKE2b-256("camex network v1" || name) */
void netw_id_from_name(const char *name, uint8_t id[CAMEX_NET_ID_LEN]);

/* A name is 1..CAMEX_NET_NAME_MAX bytes without control characters */
int netw_name_valid(const char *name);

/* The pre-shared key of Noise message 1 for a client that joins by
 * network name: a fixed public value (the name travels inside the
 * message, the server's static key keeps it secret) */
void netw_handshake_key(uint8_t key[32]);

/* "net-" and the first 4 bytes of the ID in hex: names are secrets, logs
 * and status show this (or label=) instead */
void netw_default_label(const uint8_t id[CAMEX_NET_ID_LEN], char *out,
                        size_t size);

/* The network of an ID, or -1 */
int netw_find(const uint8_t id[CAMEX_NET_ID_LEN]);

/* Label of network n ("" for -1) */
const char *netw_label(int n);

/* The network whose pool holds ip_be (network order), or -1 */
int netw_of_ip(uint32_t ip_be);

/* The networks of the file after a load or reload. Listed networks keep
 * their table slot; networks clients created stay when allow_new is set,
 * the rest is deactivated. Pools are (re)derived. reserved[] are host
 * order network/prefix pairs no derived pool may overlap (the auto pool,
 * the server's tunnel network). Returns 0, or -1 with a logged reason (an
 * explicit pool overlaps another one): the table is then unchanged. */
int netw_sync(const camex_net_conf_t *conf, size_t count, int allow_new,
              uint32_t supernet, uint8_t super_prefix, uint8_t pool_prefix,
              const uint32_t *reserved, size_t reserved_count);

/* allow_new_networks: a new network for an unknown ID, with a derived
 * pool. Its slot, or -1 (the table is full, no free pool). */
int netw_create(const uint8_t id[CAMEX_NET_ID_LEN]);

/* Pool geometry of network n (host order) */
uint32_t netw_gateway(int n);
uint32_t netw_first(int n);
uint32_t netw_last(int n);

/* 1 if ip_be (network order) is a client address of network n */
int netw_has(int n, uint32_t ip_be);

/* The k-th address (network order) network n offers client_id: k = 0 is
 * the hash's choice, then +1, -1, +2, -2, ... around it, wrapping inside
 * the pool's client addresses. 0 when k is past them. */
uint32_t netw_candidate(int n, const char *client_id, unsigned int k);

/* The offset of ip_be in client_id's sequence (the k that gives it), or
 * -1 when it is not among the first CAMEX_NET_PROBES */
int netw_candidate_rank(int n, const char *client_id, uint32_t ip_be);

/* The pool of network n as "a.b.c.d/p" */
void netw_pool_text(int n, char *buf, size_t size);

/* The derived pool of an ID without the table (tests): its offset among
 * the supernet's pools before probing */
uint32_t netw_pool_index(const uint8_t id[CAMEX_NET_ID_LEN],
                         uint32_t pools);

#endif /* CAMEX_NETW_H */
