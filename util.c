/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * util.c — shared utility functions
 *
 */

#include "util.h"
#include "camex.h"
#include "log.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#else
#include <windows.h>
#include <bcrypt.h>
#include <ws2tcpip.h>
#endif
#if defined(__APPLE__)
#include <CommonCrypto/CommonRandom.h>
#endif

void write_be64(uint8_t *dst, uint64_t val)
{
    dst[0] = (uint8_t)(val >> 56);
    dst[1] = (uint8_t)(val >> 48);
    dst[2] = (uint8_t)(val >> 40);
    dst[3] = (uint8_t)(val >> 32);
    dst[4] = (uint8_t)(val >> 24);
    dst[5] = (uint8_t)(val >> 16);
    dst[6] = (uint8_t)(val >> 8);
    dst[7] = (uint8_t)val;
}

uint64_t read_be64(const uint8_t *src)
{
    return ((uint64_t)src[0] << 56) | ((uint64_t)src[1] << 48) |
           ((uint64_t)src[2] << 40) | ((uint64_t)src[3] << 32) |
           ((uint64_t)src[4] << 24) | ((uint64_t)src[5] << 16) |
           ((uint64_t)src[6] << 8) | (uint64_t)src[7];
}

void hex_encode(const uint8_t *in, size_t len, char *out, size_t out_size)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;

    if (out == NULL || out_size == 0U) {
        return;
    }

    if (len * 2U + 1U > out_size) {
        out[0] = '\0';
        return;
    }

    for (i = 0; i < len; ++i) {
        out[i * 2U] = hex[in[i] >> 4];
        out[i * 2U + 1U] = hex[in[i] & 0x0fU];
    }
    out[len * 2U] = '\0';
}

int get_random_bytes(void *buf, size_t len)
{
    ssize_t ret;
    int fd;

#ifdef _WIN32
    (void)ret;
    (void)fd;
    if (BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0) {
        return 0;
    }
    return -1;
#elif defined(__APPLE__)
    (void)ret;
    (void)fd;
    if (CCRandomGenerateBytes(buf, len) == kCCSuccess) {
        return 0;
    }
    return -1;
#else
#ifdef SYS_getrandom
    ret = syscall(SYS_getrandom, buf, len, 0);
    if (ret == (ssize_t)len) {
        return 0;
    }
#endif
    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        print_errno_message(LOG_ERR, "open(/dev/urandom)");
        return -1;
    }
    ret = read(fd, buf, (size_t)len);
    close(fd);
    return (ret == (ssize_t)len) ? 0 : -1;
#endif
}

void trim_whitespace(char *text)
{
    char *start;
    char *end;

    if (text == NULL) {
        return;
    }

    start = text;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        ++start;
    }
    if (start != text) {
        memmove(text, start, strlen(start) + 1U);
    }

    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        --end;
    }
    *end = '\0';
}

int validate_ipv4(const char *value, const char *label)
{
    struct in_addr addr;

    if (value == NULL || inet_pton(AF_INET, value, &addr) != 1) {
        log_message(LOG_ERR, "Invalid %s: %s",
                    label, value != NULL ? value : "(null)");
        return -1;
    }

    return 0;
}

int validate_route_cidr(const char *cidr)
{
    char buffer[32];
    char *slash;
    char *end = NULL;
    long prefix;
    struct in_addr addr;

    if (cidr == NULL || *cidr == '\0') {
        return 0;
    }

    if (strlen(cidr) >= sizeof(buffer)) {
        log_message(LOG_ERR, "Route CIDR is too long");
        return -1;
    }

    snprintf(buffer, sizeof(buffer), "%s", cidr);
    slash = strchr(buffer, '/');
    if (slash == NULL || slash == buffer || slash[1] == '\0') {
        log_message(LOG_ERR, "Route CIDR must use address/prefix form: %s", cidr);
        return -1;
    }

    *slash = '\0';
    if (inet_pton(AF_INET, buffer, &addr) != 1) {
        log_message(LOG_ERR, "Invalid route network: %s", buffer);
        return -1;
    }

    errno = 0;
    prefix = strtol(slash + 1, &end, 10);
    if (errno != 0 || end == slash + 1 || *end != '\0' || prefix < 0 || prefix > 32) {
        log_message(LOG_ERR, "Invalid route prefix: %s", slash + 1);
        return -1;
    }

    return 0;
}

int validate_route_list(const char routes[][32], uint8_t route_count)
{
    size_t i;

    for (i = 0; i < route_count; ++i) {
        if (validate_route_cidr(routes[i]) != 0) {
            return -1;
        }
    }

    return 0;
}

int parse_local_cidr(const char *cidr, char *ip, size_t ip_size,
                     char *netmask, size_t netmask_size)
{
    char buffer[32];
    char *slash;
    char *end = NULL;
    long prefix;
    uint32_t mask;
    struct in_addr addr;
    struct in_addr mask_addr;

    if (cidr == NULL || ip == NULL || netmask == NULL || *cidr == '\0') {
        log_message(LOG_ERR, "Missing local CIDR");
        return -1;
    }

    if (strlen(cidr) >= sizeof(buffer)) {
        log_message(LOG_ERR, "Local CIDR is too long");
        return -1;
    }

    snprintf(buffer, sizeof(buffer), "%s", cidr);
    slash = strchr(buffer, '/');
    if (slash == NULL || slash == buffer || slash[1] == '\0') {
        log_message(LOG_ERR, "Local CIDR must use address/prefix form: %s", cidr);
        return -1;
    }

    *slash = '\0';
    if (inet_pton(AF_INET, buffer, &addr) != 1) {
        log_message(LOG_ERR, "Invalid local IP: %s", buffer);
        return -1;
    }

    errno = 0;
    prefix = strtol(slash + 1, &end, 10);
    if (errno != 0 || end == slash + 1 || *end != '\0' || prefix < 0 || prefix > 32) {
        log_message(LOG_ERR, "Invalid local prefix: %s", slash + 1);
        return -1;
    }

    mask = (prefix == 0) ? 0U : (~0U << (32U - (uint32_t)prefix));
    mask_addr.s_addr = htonl(mask);

    if (snprintf(ip, ip_size, "%s", buffer) >= (int)ip_size ||
        inet_ntop(AF_INET, &mask_addr, netmask, netmask_size) == NULL) {
        log_message(LOG_ERR, "Failed to store local CIDR");
        return -1;
    }

    return 0;
}

int parse_port(const char *value, int *port)
{
    char *end = NULL;
    long parsed;

    if (value == NULL || port == NULL || *value == '\0') {
        return -1;
    }

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < 1 || parsed > 65535) {
        return -1;
    }

    *port = (int)parsed;
    return 0;
}

int parse_mtu(const char *value, int *mtu)
{
    char *end = NULL;
    long parsed;

    if (value == NULL || mtu == NULL || *value == '\0') {
        return -1;
    }

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < 576 || parsed > 9000) {
        return -1;
    }

    *mtu = (int)parsed;
    return 0;
}

int copy_option(char *dest, size_t dest_size, const char *value, const char *label)
{
    if (dest == NULL || dest_size == 0U || value == NULL || *value == '\0') {
        log_message(LOG_ERR, "Missing %s", label);
        return -1;
    }

    if (strlen(value) >= dest_size) {
        log_message(LOG_ERR, "%s is too long", label);
        return -1;
    }

    snprintf(dest, dest_size, "%s", value);
    return 0;
}

int append_route_option(char routes[][32], uint8_t *count,
                        const char *value, const char *label)
{
    if (routes == NULL || count == NULL || value == NULL || *value == '\0') {
        log_message(LOG_ERR, "Missing %s", label);
        return -1;
    }

    if (*count >= CAMEX_MAX_ROUTES) {
        log_message(LOG_ERR, "Too many %s entries", label);
        return -1;
    }

    if (validate_route_cidr(value) != 0) {
        return -1;
    }

    if (strlen(value) >= sizeof(routes[0])) {
        log_message(LOG_ERR, "%s is too long", label);
        return -1;
    }

    snprintf(routes[*count], sizeof(routes[0]), "%s", value);
    (*count)++;
    return 0;
}

int append_control_field(char *buffer, size_t size, size_t *offset,
                         const char *fmt, ...)
{
    va_list ap;
    int written;

    if (buffer == NULL || offset == NULL || fmt == NULL || *offset >= size) {
        return -1;
    }

    va_start(ap, fmt);
    written = vsnprintf(buffer + *offset, size - *offset, fmt, ap);
    va_end(ap);
    if (written < 0 || (size_t)written >= size - *offset) {
        return -1;
    }

    *offset += (size_t)written;
    return 0;
}

int token_value(const char *token, const char *key,
                char *value, size_t value_size)
{
    size_t key_len;

    if (token == NULL || key == NULL || value == NULL || value_size == 0U) {
        return -1;
    }

    key_len = strlen(key);
    if (strncmp(token, key, key_len) != 0 || token[key_len] != '=') {
        return -1;
    }

    return copy_option(value, value_size, token + key_len + 1U, key);
}

int set_fd_nonblocking(int fd)
{
#ifndef _WIN32
    int flags;

    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }

    return 0;
#else
    u_long mode = 1;
    return (ioctlsocket(fd, FIONBIO, &mode) == 0) ? 0 : -1;
#endif
}

int is_zero_key(const uint8_t key[32])
{
    size_t i;
    for (i = 0U; i < 32U; ++i) {
        if (key[i] != 0U) {
            return 0;
        }
    }
    return 1;
}

int ipv4_parse_endpoints(const uint8_t *packet, size_t len,
                         uint32_t *src_ip, uint32_t *dst_ip)
{
    uint8_t version;
    uint8_t ihl;
    size_t hdr_len;

    if (packet == NULL || src_ip == NULL || dst_ip == NULL || len < 20U) {
        return -1;
    }

    version = packet[0] >> 4;
    ihl = packet[0] & 0x0fU;
    hdr_len = (size_t)ihl * 4U;
    if (version != 4U || ihl < 5U || len < hdr_len) {
        return -1;
    }

    memcpy(src_ip, packet + 12, sizeof(*src_ip));
    memcpy(dst_ip, packet + 16, sizeof(*dst_ip));
    return 0;
}
