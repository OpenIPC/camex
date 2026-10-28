/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * proto.h — control protocol (REGISTER / CONFIG message builders and parsers)
 *
 */

#ifndef CAMEX_PROTO_H
#define CAMEX_PROTO_H

#include "camex.h"
#include "config.h"  /* for camex_profile_t */

#include <stdint.h>
#include <stddef.h>

/* First word of the control messages (handshake payloads) */
#define CAMEX_CTRL_WORD "CAMEX"

#define CAMEX_CLIENT_TIMEOUT    120   /* the default; client_timeout= (#143) */
#define CAMEX_CLIENT_TIMEOUT_MIN 60   /* client_timeout='s lowest */
/* A client's longest keepalive interval: a third of the lowest
 * client_timeout=, so that it stays alive against any server (#143) */
#define CAMEX_KEEPALIVE_MAX     (CAMEX_CLIENT_TIMEOUT_MIN / 3)
#define CAMEX_SERVER_TIMEOUT     20
#define CAMEX_RECONNECT_INTERVAL  5
/* TCP connections that have not registered yet (#9): they hold a client
 * slot only briefly, and their number is limited overall and per address. */
#define CAMEX_TCP_PREAUTH_TIMEOUT 10
#define CAMEX_TCP_PREAUTH_MAX     32
#define CAMEX_TCP_PREAUTH_PER_IP   4
/* A new pre-auth connection is not closed to make room for 2 s: its
 * REGISTER may already be queued behind the accept backlog. */
#define CAMEX_TCP_PREAUTH_GRACE    2
#define CAMEX_CLIENT_TOKEN_LEN 64
#define CAMEX_CONTROL_MAX 1024U

/* Build REGISTER text message */
int proto_build_register(const camex_config_t *config,
                         char *buffer, size_t size);

/* Parse REGISTER text message */
int proto_parse_register(char *message,
                         char *client_id, size_t client_id_size,
                         char *local_ip, size_t local_ip_size,
                         uint8_t *auto_request);

/* Build CONFIG text message */
int proto_build_config(const char *client_id,
                       const camex_profile_t *profile,
                       char *buffer, size_t size);

/* Parse CONFIG text message into a config */
int proto_parse_config(char *message, camex_config_t *config);

#endif /* CAMEX_PROTO_H */
