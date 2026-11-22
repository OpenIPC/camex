/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * noserver.c — the server's interface (server.h) in a camex built without
 * the server (CAMEX_SERVER=0, PROFILE=client; #204). validate_config()
 * refuses --mode server, so none of this is reached at run time: it only
 * lets camex.c and control.c link without server.c.
 *
 */

#include "server.h"

#include <stddef.h>

size_t server_tcp_reg_cap = 0U;
server_client_t *server_clients = NULL;
size_t server_max_clients = 0U;

int server_clients_init(size_t count)
{
    (void)count;
    return -1;
}

void server_clients_free(void)
{
}

size_t server_default_max_clients(void)
{
    return 0U;
}

void server_check_pool(const char *server_cidr)
{
    (void)server_cidr;
}

void server_set_ip(server_client_t *entry, uint32_t ip_be)
{
    (void)entry;
    (void)ip_be;
}

void server_set_id(server_client_t *entry, const char *client_id)
{
    (void)entry;
    (void)client_id;
}

void server_set_addr(server_client_t *entry, const struct sockaddr_in *addr)
{
    (void)entry;
    (void)addr;
}

server_client_t *server_find_by_ip(uint32_t ip_be)
{
    (void)ip_be;
    return NULL;
}

server_client_t *server_find_by_client_id(const char *client_id)
{
    (void)client_id;
    return NULL;
}

server_client_t *server_find_by_addr(const struct sockaddr_in *addr, int tcp)
{
    (void)addr;
    (void)tcp;
    return NULL;
}

server_client_t *server_alloc_client(void)
{
    return NULL;
}

void server_end_client(server_client_t *entry)
{
    (void)entry;
}

void server_drop_entry(server_client_t *entry)
{
    (void)entry;
}

void server_expire_clients(void)
{
}

void server_revalidate_keys(void)
{
}

void server_revalidate_unknown_clients(void)
{
}

void server_revalidate_profile_addresses(void)
{
}

void server_networks_apply(void)
{
}

int server_networks_sync(void)
{
    return -1;
}

void server_networks_down(void)
{
}

void server_rates_tick(void)
{
}

int server_forward_packet(const uint8_t *packet, size_t len,
                          uint32_t src_ip_be, uint32_t dst_ip_be)
{
    (void)packet;
    (void)len;
    (void)src_ip_be;
    (void)dst_ip_be;
    return -1;
}

int server_handle_packet(const uint8_t *buffer, size_t len,
                         const struct sockaddr_in *from, int fd)
{
    (void)buffer;
    (void)len;
    (void)from;
    (void)fd;
    return -1;
}

int server_socket_create(const char *bind_ip, int port, const char *bind_dev)
{
    (void)bind_ip;
    (void)port;
    (void)bind_dev;
    return -1;
}

/* NULL: control.c closes the connection without an answer */
char *server_status(int json, size_t *len)
{
    (void)json;
    (void)len;
    return NULL;
}

char *server_clients_list(int names, int web, size_t *len)
{
    (void)names;
    (void)web;
    (void)len;
    return NULL;
}

char *server_clients_index(int via, size_t *len)
{
    (void)via;
    (void)len;
    return NULL;
}

uint64_t server_state_hash(void)
{
    return 0U;
}
