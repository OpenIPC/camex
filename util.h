/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * util.h — shared utility functions (endian, hex, validation, etc.)
 *
 */

#ifndef CAMEX_UTIL_H
#define CAMEX_UTIL_H

#include <stdint.h>
#include <stddef.h>

/* Big-endian helpers */
void write_be64(uint8_t *dst, uint64_t val);
uint64_t read_be64(const uint8_t *src);

/* Hex encoding */
void hex_encode(const uint8_t *in, size_t len, char *out, size_t out_size);

/* Secure random bytes */
int get_random_bytes(void *buf, size_t len);

/* String trimming */
void trim_whitespace(char *text);

/* Validation helpers */
int validate_ipv4(const char *value, const char *label);
int validate_route_cidr(const char *cidr);
int validate_route_list(const char routes[][32], uint8_t route_count);
int parse_local_cidr(const char *cidr, char *ip, size_t ip_size,
                     char *netmask, size_t netmask_size);
int parse_port(const char *value, int *port);
int parse_mtu(const char *value, int *mtu);

/* Option copy helpers */
int copy_option(char *dest, size_t dest_size, const char *value,
                const char *label);
int append_route_option(char routes[][32], uint8_t *count,
                        const char *value, const char *label);

/* Control message builder helper */
int append_control_field(char *buffer, size_t size, size_t *offset,
                         const char *fmt, ...);

/* Token parser */
int token_value(const char *token, const char *key,
                char *value, size_t value_size);

/* FD helpers */
int set_fd_nonblocking(int fd);

/* Zero-key check */
int is_zero_key(const uint8_t key[32]);

/* IPv4 parse from raw packet */
int ipv4_parse_endpoints(const uint8_t *packet, size_t len,
                         uint32_t *src_ip, uint32_t *dst_ip);

#endif /* CAMEX_UTIL_H */
