/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * camex.c — client/server UDP TUN tunnel
 *
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netdb.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/syslog.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <net/route.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#include "camex.h"
#include "monocypher.h"
#include "version.h"

#define CAMEX_MAGIC "CX2P"
#define CAMEX_PACKET_REGISTER 1U
#define CAMEX_PACKET_DATA 2U
#define CAMEX_PACKET_CONFIG 3U
#define CAMEX_HDR_LEN 27U
#define CAMEX_REGISTER_INTERVAL  10
#define CAMEX_CLIENT_TIMEOUT    120
#define CAMEX_SERVER_TIMEOUT     20  /* seconds of silence before reconnecting */
#define CAMEX_RECONNECT_INTERVAL  5  /* seconds between reconnect attempts */
#define CAMEX_MAX_CLIENTS 256
#define CAMEX_CLIENT_TOKEN_LEN 64
#define CAMEX_CONTROL_MAX 1024U
#define CAMEX_DEFAULT_CONFIG_PATH "/etc/camex/camex.conf"

#ifndef TUN_PACKET_MAX
#define TUN_PACKET_MAX 9216U
#endif

typedef struct {
    uint8_t psk_key[32];
    uint8_t fingerprint[16];
    uint8_t ready;
} camex_crypto_t;

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
    uint8_t psk_key[32]; /* derived key from per-client PSK; zero = use global */
} server_client_t;

/* Keystore: one entry per unique PSK (global + per-client) */
typedef struct {
    uint8_t psk_key[32];
} camex_keystore_entry_t;

typedef struct {
    uint64_t send_seq;
    uint64_t recv_seq_max;
    uint64_t recv_window;
    time_t last_register;
    time_t last_recv;   /* timestamp of last packet received from server */
    uint8_t registered;
    uint8_t config_received; /* set after first CONFIG; cleared on reconnect to re-request */
    uint8_t send_nonce_prefix[4];
} client_state_t;

typedef struct {
    char local_cidr[32];
    char gateway_ip[16];
    char route_cidrs[CAMEX_MAX_ROUTES][32];
    uint8_t route_count;
    int mtu;
    char psk[64];   /* per-client pre-shared key (optional; plaintext, wiped after key derivation) */
} camex_profile_t;

typedef struct {
    char client_id[CAMEX_CLIENT_ID_LEN];
    camex_profile_t profile;
    uint8_t active;
} camex_server_profile_t;

/* Server-level parameters from the [server] section of camex.conf */
typedef struct {
    char bind_ip[16];
    char local_cidr[32];
    char tun_dev[256];
    char bind_dev[IFNAMSIZ];
    char pid_file[256];
    char psk[64];
    int  port;
    int  mtu;
    uint8_t encrypt;
    uint8_t encrypt_set;  /* 1 = encrypt was explicitly set in config file */
} camex_server_globals_t;

typedef struct {
    camex_profile_t defaults;
    camex_server_profile_t clients[CAMEX_MAX_CLIENTS];
    camex_server_globals_t globals;
    uint8_t loaded;
} camex_server_db_t;

static int tun_fd = -1;
static int udp_socket = -1;
static char tun_name[IFNAMSIZ];
static struct sockaddr_in server_addr;
static volatile sig_atomic_t running = 1;
static camex_config_t current_config;
static camex_crypto_t crypto_ctx;
static client_state_t client_state;
static server_client_t server_clients[CAMEX_MAX_CLIENTS];
static camex_server_db_t server_db;
static uint8_t client_mode = 0;
static uint8_t server_mode = 0;
static volatile sig_atomic_t reload_config = 0;
static time_t g_now = 0;
static time_t client_reconnect_at = 0; /* schedule next reconnect attempt */
static uint8_t client_link_up = 0;     /* 1 = server connection is considered up */
static camex_keystore_entry_t server_keystore[CAMEX_MAX_CLIENTS + 1];
static size_t server_keystore_count = 0U;

static void print_usage(const char *progname);
static void print_version(void);
static int validate_route_cidr(const char *cidr);
static int build_register_message(camex_config_t *config, char *buffer, size_t size);
static int parse_register_message(char *message, char *client_id, size_t client_id_size, char *local_ip, size_t local_ip_size, uint8_t *auto_request);
static int build_config_message(const char *client_id, const camex_profile_t *profile, char *buffer, size_t size);
static int parse_config_message(char *message, camex_config_t *config);
static int set_fd_nonblocking(int fd);
static int tune_udp_socket(int fd);
static int client_socket_create(const char *host, int port);

static void log_message(int priority, const char *fmt, ...)
{
    char message[512];
    va_list ap;
    FILE *stream = (priority <= LOG_WARNING) ? stderr : stdout;

    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    syslog(priority, "%s", message);
    fprintf(stream, "%s\n", message);
}

static void print_errno_message(int priority, const char *what)
{
    log_message(priority, "%s: %s", what, strerror(errno));
}

static void write_be64(uint8_t *dst, uint64_t val)
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

static uint64_t read_be64(const uint8_t *src)
{
    return ((uint64_t)src[0] << 56) | ((uint64_t)src[1] << 48) |
           ((uint64_t)src[2] << 40) | ((uint64_t)src[3] << 32) |
           ((uint64_t)src[4] << 24) | ((uint64_t)src[5] << 16) |
           ((uint64_t)src[6] << 8) | (uint64_t)src[7];
}

static int replay_check(uint64_t *seq_max, uint64_t *window, uint64_t seq)
{
    uint64_t diff;

    if (seq > *seq_max) {
        diff = seq - *seq_max;
        *window = (diff < 64U) ? ((*window << diff) | 1U) : 1U;
        *seq_max = seq;
        return 0;
    }

    diff = *seq_max - seq;
    if (diff >= 64U || ((*window >> diff) & 1U) != 0U) {
        return -1;
    }

    *window |= ((uint64_t)1 << diff);
    return 0;
}

static void hex_encode(const uint8_t *in, size_t len, char *out, size_t out_size)
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

static void derive_psk_key(const char *psk, uint8_t key[32])
{
    if (psk == NULL) {
        memset(key, 0, 32);
        return;
    }

    crypto_blake2b(key, 32, (const uint8_t *)psk, strlen(psk));
}

static void crypto_log_fingerprint(void)
{
    char fp[33];

    hex_encode(crypto_ctx.fingerprint, sizeof(crypto_ctx.fingerprint), fp, sizeof(fp));
    log_message(LOG_INFO, "Encryption fingerprint: %s", fp);
}

static int crypto_init(const char *psk)
{
    if (psk == NULL || *psk == '\0') {
        log_message(LOG_ERR, "Encryption requested but PSK is empty");
        return -1;
    }

    derive_psk_key(psk, crypto_ctx.psk_key);
    crypto_blake2b(crypto_ctx.fingerprint, sizeof(crypto_ctx.fingerprint), crypto_ctx.psk_key, sizeof(crypto_ctx.psk_key));
    crypto_ctx.ready = 1U;
    crypto_log_fingerprint();
    return 0;
}

static int crypto_encrypt_packet(uint8_t type, uint64_t seq, const uint8_t nonce_prefix[4], const uint8_t *key, const uint8_t *plain, size_t plain_len, uint8_t *packet, size_t packet_size, size_t *packet_len)
{
    crypto_aead_ctx ctx;
    uint8_t nonce[12];
    uint16_t body_len;

    if (!crypto_ctx.ready || packet == NULL || packet_len == NULL || (plain_len > 0U && plain == NULL)) {
        return -1;
    }

    if (plain_len > 65535U || packet_size < CAMEX_HDR_LEN + plain_len + 16U) {
        return -1;
    }

    memcpy(nonce, nonce_prefix, 4);
    write_be64(nonce + 4, seq);

    memcpy(packet, CAMEX_MAGIC, 4);
    packet[4] = type;
    memcpy(packet + 5, nonce, 12);
    body_len = htons((uint16_t)plain_len);
    memcpy(packet + 17, &body_len, sizeof(body_len));
    write_be64(packet + 19, seq);

    crypto_aead_init_ietf(&ctx, key != NULL ? key : crypto_ctx.psk_key, nonce);
    crypto_aead_write(&ctx, packet + CAMEX_HDR_LEN, packet + CAMEX_HDR_LEN + plain_len, packet, CAMEX_HDR_LEN, plain, plain_len);
    *packet_len = CAMEX_HDR_LEN + plain_len + 16U;
    crypto_wipe(nonce, sizeof(nonce));
    return 0;
}

static int crypto_decrypt_packet(const uint8_t *packet, size_t packet_len, uint8_t *type, uint8_t *plain, size_t plain_size, size_t *plain_len, uint64_t *seq_out, const uint8_t *key)
{
    crypto_aead_ctx ctx;
    uint8_t nonce[12];
    uint16_t body_len_be;
    size_t body_len;

    if (!crypto_ctx.ready || packet == NULL || type == NULL || plain_len == NULL) {
        return -1;
    }

    if (packet_len < CAMEX_HDR_LEN + 16U || memcmp(packet, CAMEX_MAGIC, 4) != 0) {
        return -1;
    }

    *type = packet[4];
    memcpy(nonce, packet + 5, 12);
    memcpy(&body_len_be, packet + 17, sizeof(body_len_be));
    body_len = (size_t)ntohs(body_len_be);

    if (packet_len != CAMEX_HDR_LEN + body_len + 16U || body_len > plain_size) {
        return -1;
    }

    crypto_aead_init_ietf(&ctx, key != NULL ? key : crypto_ctx.psk_key, nonce);
    if (crypto_aead_read(&ctx, plain, packet + CAMEX_HDR_LEN + body_len, packet, CAMEX_HDR_LEN, packet + CAMEX_HDR_LEN, body_len) != 0) {
        return -1;
    }

    if (seq_out != NULL) {
        *seq_out = read_be64(packet + 19);
    }

    *plain_len = body_len;
    crypto_wipe(nonce, sizeof(nonce));
    return 0;
}

/* Returns 1 if all 32 bytes of a key are zero (i.e., unset). */
static int is_zero_key(const uint8_t key[32])
{
    size_t i;
    for (i = 0U; i < 32U; ++i) {
        if (key[i] != 0U) {
            return 0;
        }
    }
    return 1;
}

static int parse_port(const char *value, int *port)
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

static int parse_mtu(const char *value, int *mtu)
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

static int copy_option(char *dest, size_t dest_size, const char *value, const char *label)
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

static int append_route_option(char routes[CAMEX_MAX_ROUTES][32], uint8_t *count, const char *value, const char *label)
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

static int validate_ipv4(const char *value, const char *label)
{
    struct in_addr addr;

    if (value == NULL || inet_pton(AF_INET, value, &addr) != 1) {
        log_message(LOG_ERR, "Invalid %s: %s", label, value != NULL ? value : "(null)");
        return -1;
    }

    return 0;
}

static int parse_local_cidr(const char *cidr, char *ip, size_t ip_size, char *netmask, size_t netmask_size)
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

static int validate_route_cidr(const char *cidr)
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

static int set_fd_nonblocking(int fd)
{
    int flags;

    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }

    return 0;
}

static int tune_udp_socket(int fd)
{
    int bufsize;

    bufsize = 4 * 1024 * 1024;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize)) < 0) {
        print_errno_message(LOG_WARNING, "setsockopt(SO_RCVBUF)");
    }
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize)) < 0) {
        print_errno_message(LOG_WARNING, "setsockopt(SO_SNDBUF)");
    }
    return set_fd_nonblocking(fd);
}

static int get_random_bytes(void *buf, size_t len)
{
    ssize_t ret;
    int fd;

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
}

static void trim_whitespace(char *text)
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

static void profile_reset(camex_profile_t *profile)
{
    if (profile == NULL) {
        return;
    }

    memset(profile, 0, sizeof(*profile));
    profile->mtu = 0;
}

static int profile_append_route(camex_profile_t *profile, const char *value)
{
    if (profile == NULL) {
        return -1;
    }

    return append_route_option(profile->route_cidrs, &profile->route_count, value, "route CIDR");
}

static void profile_merge(camex_profile_t *dst, const camex_profile_t *src)
{
    size_t i;

    if (dst == NULL || src == NULL) {
        return;
    }

    if (src->local_cidr[0] != '\0') {
        snprintf(dst->local_cidr, sizeof(dst->local_cidr), "%s", src->local_cidr);
    }
    if (src->gateway_ip[0] != '\0') {
        snprintf(dst->gateway_ip, sizeof(dst->gateway_ip), "%s", src->gateway_ip);
    }
    if (src->mtu > 0) {
        dst->mtu = src->mtu;
    }
    /* psk is intentionally NOT merged from defaults — it is per-client only */
    for (i = 0; i < src->route_count && dst->route_count < CAMEX_MAX_ROUTES; ++i) {
        snprintf(dst->route_cidrs[dst->route_count], sizeof(dst->route_cidrs[0]), "%s", src->route_cidrs[i]);
        ++dst->route_count;
    }
}

static void server_db_reset(void)
{
    size_t i;

    memset(&server_db, 0, sizeof(server_db));
    profile_reset(&server_db.defaults);
    for (i = 0; i < CAMEX_MAX_CLIENTS; ++i) {
        profile_reset(&server_db.clients[i].profile);
    }
}

static camex_server_profile_t *server_db_find_client(const char *client_id)
{
    size_t i;

    if (client_id == NULL || *client_id == '\0') {
        return NULL;
    }

    for (i = 0; i < CAMEX_MAX_CLIENTS; ++i) {
        if (server_db.clients[i].active && strcasecmp(server_db.clients[i].client_id, client_id) == 0) {
            return &server_db.clients[i];
        }
    }

    return NULL;
}

static camex_server_profile_t *server_db_get_client_slot(const char *client_id)
{
    size_t i;
    camex_server_profile_t *slot = NULL;

    if (client_id == NULL || *client_id == '\0') {
        return NULL;
    }

    slot = server_db_find_client(client_id);
    if (slot != NULL) {
        return slot;
    }

    for (i = 0; i < CAMEX_MAX_CLIENTS; ++i) {
        if (!server_db.clients[i].active) {
            slot = &server_db.clients[i];
            memset(slot, 0, sizeof(*slot));
            slot->active = 1U;
            {
                size_t id_len = strlen(client_id);
                if (id_len >= sizeof(slot->client_id)) {
                    log_message(LOG_ERR, "Client ID is too long: %s", client_id);
                    return NULL;
                }
                /* Use memcpy — length already verified; avoids -Wformat-truncation */
                memcpy(slot->client_id, client_id, id_len + 1U);
            }
            profile_reset(&slot->profile);
            return slot;
        }
    }

    return NULL;
}

static int server_db_apply_kv(camex_profile_t *profile, const char *key, const char *value)
{
    if (profile == NULL || key == NULL || value == NULL) {
        return -1;
    }

    if (strcmp(key, "local_cidr") == 0) {
        return copy_option(profile->local_cidr, sizeof(profile->local_cidr), value, "local CIDR");
    }
    if (strcmp(key, "gateway_ip") == 0) {
        return copy_option(profile->gateway_ip, sizeof(profile->gateway_ip), value, "gateway IP");
    }
    if (strcmp(key, "route_cidr") == 0) {
        return profile_append_route(profile, value);
    }
    if (strcmp(key, "mtu") == 0) {
        return parse_mtu(value, &profile->mtu);
    }
    if (strcmp(key, "psk") == 0) {
        return copy_option(profile->psk, sizeof(profile->psk), value, "per-client PSK");
    }

    log_message(LOG_WARNING, "Ignoring unknown config key: %s", key);
    return 0;
}

static int server_db_load_file(const char *path)
{
    FILE *fp;
    char line[512];
    camex_profile_t *current = NULL;
    camex_server_profile_t *client = NULL;
    enum {
        SECTION_NONE = 0,
        SECTION_DEFAULTS,
        SECTION_CLIENT,
        SECTION_SERVER
    } section = SECTION_NONE;

    server_db_reset();

    if (path == NULL || *path == '\0') {
        return 0;
    }

    fp = fopen(path, "r");
    if (fp == NULL) {
        if (errno == ENOENT) {
            log_message(LOG_WARNING, "Config file not found: %s", path);
            return 0;
        }
        print_errno_message(LOG_ERR, "fopen(config)");
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *content;
        char *equals;
        trim_whitespace(line);
        if (*line == '\0' || *line == '#' || *line == ';') {
            continue;
        }

        if (line[0] == '[') {
            size_t len = strlen(line);

            if (len < 2U || line[len - 1U] != ']') {
                log_message(LOG_ERR, "Invalid config section: %s", line);
                fclose(fp);
                return -1;
            }

            line[len - 1U] = '\0';
            content = line + 1;
            trim_whitespace(content);

            if (strcasecmp(content, "server") == 0) {
                section = SECTION_SERVER;
                current = NULL;
                client = NULL;
                continue;
            }

            if (strcasecmp(content, "defaults") == 0) {
                section = SECTION_DEFAULTS;
                current = &server_db.defaults;
                client = NULL;
                continue;
            }

            if (strncasecmp(content, "client", 6) == 0 && (content[6] == '\0' || isspace((unsigned char)content[6]))) {
                char *client_id = content + 6;

                trim_whitespace(client_id);
                if (*client_id == '\0') {
                    log_message(LOG_ERR, "Missing client ID in section: %s", line);
                    fclose(fp);
                    return -1;
                }

                client = server_db_get_client_slot(client_id);
                if (client == NULL) {
                    log_message(LOG_ERR, "No free slots for client profile: %s", client_id);
                    fclose(fp);
                    return -1;
                }

                section = SECTION_CLIENT;
                current = &client->profile;
                continue;
            }

            log_message(LOG_ERR, "Unknown config section: %s", content);
            fclose(fp);
            return -1;
        }

        /* Key=value parsing */
        equals = strchr(line, '=');
        if (equals == NULL) {
            log_message(LOG_ERR, "Invalid config line: %s", line);
            fclose(fp);
            return -1;
        }

        *equals = '\0';
        content = line;
        trim_whitespace(content);
        trim_whitespace(equals + 1);

        if (*content == '\0' || *(equals + 1) == '\0') {
            log_message(LOG_ERR, "Invalid config line: %s=%s", content, equals + 1);
            fclose(fp);
            return -1;
        }

        if (section == SECTION_SERVER) {
            camex_server_globals_t *g = &server_db.globals;
            const char *val = equals + 1;
            int rc = 0;

            if (strcmp(content, "port") == 0) {
                rc = parse_port(val, &g->port);
            } else if (strcmp(content, "bind_ip") == 0) {
                rc = copy_option(g->bind_ip, sizeof(g->bind_ip), val, "bind IP");
            } else if (strcmp(content, "local_cidr") == 0) {
                rc = copy_option(g->local_cidr, sizeof(g->local_cidr), val, "local CIDR");
            } else if (strcmp(content, "mtu") == 0) {
                rc = parse_mtu(val, &g->mtu);
            } else if (strcmp(content, "encrypt") == 0) {
                if (strcasecmp(val, "yes") == 0 || strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0) {
                    g->encrypt = 1U;
                } else if (strcasecmp(val, "no") == 0 || strcasecmp(val, "false") == 0 || strcmp(val, "0") == 0) {
                    g->encrypt = 0U;
                } else {
                    log_message(LOG_ERR, "Invalid value for encrypt: %s", val);
                    rc = -1;
                }
                g->encrypt_set = 1U;
            } else if (strcmp(content, "psk") == 0) {
                rc = copy_option(g->psk, sizeof(g->psk), val, "PSK");
            } else if (strcmp(content, "tun_dev") == 0) {
                rc = copy_option(g->tun_dev, sizeof(g->tun_dev), val, "TUN device");
            } else if (strcmp(content, "bind_dev") == 0) {
                rc = copy_option(g->bind_dev, sizeof(g->bind_dev), val, "bind device");
            } else if (strcmp(content, "pid_file") == 0) {
                rc = copy_option(g->pid_file, sizeof(g->pid_file), val, "PID file");
            } else {
                log_message(LOG_WARNING, "Ignoring unknown [server] key: %s", content);
            }

            if (rc != 0) {
                fclose(fp);
                return -1;
            }
            continue;
        }

        if (current == NULL) {
            log_message(LOG_ERR, "Config value outside of a section: %s", line);
            fclose(fp);
            return -1;
        }

        if (server_db_apply_kv(current, content, equals + 1) != 0) {
            fclose(fp);
            return -1;
        }
    }

    fclose(fp);
    server_db.loaded = 1U;
    (void)section;
    return 0;
}

static int server_db_lookup_profile(const char *client_id, camex_profile_t *profile)
{
    camex_server_profile_t *client;

    if (profile == NULL) {
        return -1;
    }

    *profile = server_db.defaults;
    client = server_db_find_client(client_id);
    if (client != NULL) {
        profile_merge(profile, &client->profile);
    }

    return 0;
}

static int read_default_gateway_interface(char *ifname, size_t ifname_size)
{
    FILE *fp;
    char line[256];

    if (ifname == NULL || ifname_size == 0U) {
        return -1;
    }

    fp = fopen("/proc/net/route", "r");
    if (fp == NULL) {
        print_errno_message(LOG_ERR, "fopen(/proc/net/route)");
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char iface[IFNAMSIZ];
        unsigned long destination = 0;
        unsigned long gateway = 0;
        unsigned long flags = 0;

        if (sscanf(line, "%15s %lx %lx %lx", iface, &destination, &gateway, &flags) != 4) {
            continue;
        }

        if (destination == 0UL && (flags & 0x2UL) != 0UL) {
            snprintf(ifname, ifname_size, "%s", iface);
            fclose(fp);
            return 0;
        }
    }

    fclose(fp);
    log_message(LOG_ERR, "Cannot determine default gateway interface");
    return -1;
}

static int read_interface_mac(const char *ifname, uint8_t mac[6])
{
    struct ifreq ifr;
    int sock;

    if (ifname == NULL || mac == NULL || *ifname == '\0') {
        return -1;
    }

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        print_errno_message(LOG_ERR, "socket(AF_INET, SOCK_DGRAM)");
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    if (ioctl(sock, SIOCGIFHWADDR, &ifr) < 0) {
        print_errno_message(LOG_ERR, "ioctl(SIOCGIFHWADDR)");
        close(sock);
        return -1;
    }

    close(sock);

    if (ifr.ifr_hwaddr.sa_family != ARPHRD_ETHER) {
        log_message(LOG_ERR, "Interface %s has no Ethernet MAC address", ifname);
        return -1;
    }

    memcpy(mac, ifr.ifr_hwaddr.sa_data, 6U);
    if (memcmp(mac, "\0\0\0\0\0\0", 6U) == 0) {
        log_message(LOG_ERR, "Interface %s has an empty MAC address", ifname);
        return -1;
    }

    return 0;
}

static int derive_client_id(char *client_id, size_t client_id_size)
{
    char ifname[IFNAMSIZ];
    uint8_t mac[6];
    size_t i;

    if (client_id == NULL || client_id_size == 0U) {
        return -1;
    }

    if (read_default_gateway_interface(ifname, sizeof(ifname)) != 0) {
        return -1;
    }

    if (read_interface_mac(ifname, mac) != 0) {
        return -1;
    }

    if (client_id_size <= 12U) {
        return -1;
    }

    for (i = 0; i < 6U; ++i) {
        snprintf(client_id + (i * 2U), client_id_size - (i * 2U), "%02X", mac[i]);
    }
    client_id[12] = '\0';
    return 0;
}

static const char *mode_to_string(camex_mode_t mode)
{
    switch (mode) {
    case CAMEX_MODE_CLIENT:
        return "client";
    case CAMEX_MODE_SERVER:
        return "server";
    default:
        return "unknown";
    }
}

static int parse_mode(const char *value, camex_mode_t *mode)
{
    if (value == NULL || mode == NULL) {
        return -1;
    }

    if (strcmp(value, "client") == 0) {
        *mode = CAMEX_MODE_CLIENT;
        return 0;
    }

    if (strcmp(value, "server") == 0) {
        *mode = CAMEX_MODE_SERVER;
        return 0;
    }

    return -1;
}

static int resolve_udp_endpoint(const char *host, int port, struct sockaddr_in *addr, const char *label)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    char portbuf[16];

    if (host == NULL || addr == NULL || label == NULL) {
        return -1;
    }

    memset(addr, 0, sizeof(*addr));
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    snprintf(portbuf, sizeof(portbuf), "%d", port);

    if (getaddrinfo(host, portbuf, &hints, &result) != 0 || result == NULL) {
        log_message(LOG_ERR, "Cannot resolve %s: %s", label, host);
        return -1;
    }

    memcpy(addr, result->ai_addr, sizeof(*addr));
    freeaddrinfo(result);
    return 0;
}

static int sockaddr_equal(const struct sockaddr_in *lhs, const struct sockaddr_in *rhs)
{
    return lhs != NULL && rhs != NULL &&
           lhs->sin_family == rhs->sin_family &&
           lhs->sin_port == rhs->sin_port &&
           lhs->sin_addr.s_addr == rhs->sin_addr.s_addr;
}

static void sockaddr_to_string(const struct sockaddr_in *addr, char *buffer, size_t size)
{
    char ipbuf[INET_ADDRSTRLEN];
    const char *ip;

    if (buffer == NULL || size == 0U) {
        return;
    }

    if (addr == NULL) {
        snprintf(buffer, size, "(null)");
        return;
    }

    ip = inet_ntop(AF_INET, &addr->sin_addr, ipbuf, sizeof(ipbuf));
    if (ip == NULL) {
        snprintf(buffer, size, "?:%u", (unsigned)ntohs(addr->sin_port));
        return;
    }

    snprintf(buffer, size, "%s:%u", ip, (unsigned)ntohs(addr->sin_port));
}

static int send_udp_payload(int sock, const struct sockaddr_in *to, const uint8_t *data, size_t len)
{
    int sent;

    if (sock < 0 || data == NULL) {
        return -1;
    }

    if (to == NULL) {
        sent = send(sock, data, len, 0);
    } else {
        sent = sendto(sock, data, len, 0, (const struct sockaddr *)to, sizeof(*to));
    }

    if (sent < 0 || (size_t)sent != len) {
        return -1;
    }

    return 0;
}

static int send_udp_text(int sock, const struct sockaddr_in *to, const char *text)
{
    return send_udp_payload(sock, to, (const uint8_t *)text, strlen(text));
}

static int run_ip_route(const char *verb, const char *cidr, const char *gateway)
{
    struct rtentry rt;
    struct sockaddr_in *dst, *gw, *msk;
    char cidr_buf[32];
    char *slash;
    char *end = NULL;
    long prefix;
    uint32_t netmask_val;
    struct in_addr dst_addr;
    struct in_addr gw_addr;
    int sock;
    int add;
    int ret;

    if (cidr == NULL || gateway == NULL || verb == NULL) {
        return -1;
    }

    add = (strcmp(verb, "replace") == 0 || strcmp(verb, "add") == 0);

    if (strlen(cidr) >= sizeof(cidr_buf)) {
        return -1;
    }
    snprintf(cidr_buf, sizeof(cidr_buf), "%s", cidr);
    slash = strchr(cidr_buf, '/');
    if (slash == NULL || slash == cidr_buf) {
        return -1;
    }
    *slash = '\0';

    if (inet_pton(AF_INET, cidr_buf, &dst_addr) != 1) {
        return -1;
    }
    if (inet_pton(AF_INET, gateway, &gw_addr) != 1) {
        return -1;
    }

    errno = 0;
    prefix = strtol(slash + 1, &end, 10);
    if (errno != 0 || end == slash + 1 || *end != '\0' || prefix < 0 || prefix > 32) {
        return -1;
    }

    netmask_val = (prefix == 0) ? 0U : htonl(~0U << (32U - (uint32_t)prefix));

    memset(&rt, 0, sizeof(rt));
    dst = (struct sockaddr_in *)&rt.rt_dst;
    dst->sin_family = AF_INET;
    dst->sin_addr.s_addr = dst_addr.s_addr & netmask_val;

    gw = (struct sockaddr_in *)&rt.rt_gateway;
    gw->sin_family = AF_INET;
    gw->sin_addr.s_addr = gw_addr.s_addr;

    msk = (struct sockaddr_in *)&rt.rt_genmask;
    msk->sin_family = AF_INET;
    msk->sin_addr.s_addr = netmask_val;

    rt.rt_flags = RTF_UP | RTF_GATEWAY;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        print_errno_message(LOG_ERR, "socket for route");
        return -1;
    }

    ret = ioctl(sock, add ? SIOCADDRT : SIOCDELRT, &rt);
    close(sock);

    if (ret < 0) {
        if (errno == EEXIST && add) {
            return 0;
        }
        log_message(LOG_ERR, "Failed to %s route %s via %s: %s", verb, cidr, gateway, strerror(errno));
        return -1;
    }

    return 0;
}

int camex_add_route(const char *cidr, const char *gateway)
{
    return run_ip_route("replace", cidr, gateway);
}

int camex_del_route(const char *cidr, const char *gateway)
{
    return run_ip_route("del", cidr, gateway);
}

static int tun_create_device(const char *local_ip, const char *netmask, int mtu)
{
    struct ifreq ifr;
    struct ifreq cfg;
    struct sockaddr_in *sin;
    int fd = -1;
    int sock;
    int use_tunsetiff = 0;
    const char *devpath_used = NULL;
    static const char DEV_NET_TUN[] = "/dev/net/tun";
    static const char DEV_CAMEX[]   = "/dev/camex";
    const char *p;

    if (local_ip == NULL || netmask == NULL || mtu <= 0) {
        return -1;
    }

    /* Determine which TUN device path to use */
    if (current_config.tun_dev[0] != '\0') {
        /* User-specified path */
        devpath_used = current_config.tun_dev;
        log_message(LOG_INFO, "TUN device override: %s", devpath_used);
        fd = open(devpath_used, O_RDWR);
        if (fd < 0) {
            print_errno_message(LOG_ERR, "open(tun_dev)");
            return -1;
        }
    } else {
        /* Auto-detect: try /dev/net/tun (tun.ko) first */
        fd = open(DEV_NET_TUN, O_RDWR);
        if (fd >= 0) {
            /* Probe TUNSETIFF; if it fails, fall back to /dev/camex */
            memset(&ifr, 0, sizeof(ifr));
            ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
            snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "tun%%d");
            if (ioctl(fd, TUNSETIFF, (void *)&ifr) >= 0) {
                devpath_used  = DEV_NET_TUN;
                use_tunsetiff = 1;
                snprintf(tun_name, sizeof(tun_name), "%s", ifr.ifr_name);
            } else {
                /* TUNSETIFF failed — not the right backend, try camex.ko */
                close(fd);
                fd = -1;
            }
        }

        if (fd < 0) {
            fd = open(DEV_CAMEX, O_RDWR);
            if (fd < 0) {
                log_message(LOG_ERR, "No TUN backend available: /dev/net/tun and /dev/camex both failed");
                return -1;
            }
            devpath_used = DEV_CAMEX;
        }
    }

    /* For explicit path or camex.ko fallback: determine mode */
    if (!use_tunsetiff) {
        if (strcmp(devpath_used, DEV_NET_TUN) == 0) {
            /* Explicit /dev/net/tun path */
            memset(&ifr, 0, sizeof(ifr));
            ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
            snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "tun%%d");
            if (ioctl(fd, TUNSETIFF, (void *)&ifr) < 0) {
                print_errno_message(LOG_ERR, "ioctl(TUNSETIFF)");
                close(fd);
                return -1;
            }
            snprintf(tun_name, sizeof(tun_name), "%s", ifr.ifr_name);
        } else {
            /* camex.ko mode: no TUNSETIFF; derive interface name from path */
            p = strrchr(devpath_used, '/');
            strncpy(tun_name, (p != NULL) ? p + 1 : devpath_used, sizeof(tun_name) - 1U);
            tun_name[sizeof(tun_name) - 1U] = '\0';
        }
    }

    log_message(LOG_INFO, "TUN backend: %s (%s)", tun_name, devpath_used);

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        print_errno_message(LOG_ERR, "socket(AF_INET, SOCK_DGRAM)");
        close(fd);
        tun_name[0] = '\0';
        return -1;
    }

    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.ifr_name, sizeof(cfg.ifr_name), "%s", tun_name);
    sin = (struct sockaddr_in *)&cfg.ifr_addr;
    sin->sin_family = AF_INET;
    if (inet_pton(AF_INET, local_ip, &sin->sin_addr) != 1 || ioctl(sock, SIOCSIFADDR, &cfg) < 0) {
        print_errno_message(LOG_ERR, "ioctl(SIOCSIFADDR)");
        close(sock);
        close(fd);
        tun_name[0] = '\0';
        return -1;
    }

    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.ifr_name, sizeof(cfg.ifr_name), "%s", tun_name);
    sin = (struct sockaddr_in *)&cfg.ifr_netmask;
    sin->sin_family = AF_INET;
    if (inet_pton(AF_INET, netmask, &sin->sin_addr) != 1 || ioctl(sock, SIOCSIFNETMASK, &cfg) < 0) {
        print_errno_message(LOG_ERR, "ioctl(SIOCSIFNETMASK)");
        close(sock);
        close(fd);
        tun_name[0] = '\0';
        return -1;
    }

    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.ifr_name, sizeof(cfg.ifr_name), "%s", tun_name);
    cfg.ifr_mtu = mtu;
    if (ioctl(sock, SIOCSIFMTU, &cfg) < 0) {
        print_errno_message(LOG_WARNING, "ioctl(SIOCSIFMTU)");
    }

    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.ifr_name, sizeof(cfg.ifr_name), "%s", tun_name);
    if (ioctl(sock, SIOCGIFFLAGS, &cfg) < 0) {
        print_errno_message(LOG_ERR, "ioctl(SIOCGIFFLAGS)");
        close(sock);
        close(fd);
        tun_name[0] = '\0';
        return -1;
    }

    cfg.ifr_flags |= IFF_UP | IFF_RUNNING;
    if (ioctl(sock, SIOCSIFFLAGS, &cfg) < 0) {
        print_errno_message(LOG_ERR, "ioctl(SIOCSIFFLAGS)");
        close(sock);
        close(fd);
        tun_name[0] = '\0';
        return -1;
    }

    close(sock);
    tun_fd = fd;
    if (set_fd_nonblocking(tun_fd) != 0) {
        print_errno_message(LOG_WARNING, "fcntl(O_NONBLOCK)");
    }
    return 0;
}

static void tun_close_device(void)
{
    if (tun_fd >= 0) {
        close(tun_fd);
        tun_fd = -1;
    }
    tun_name[0] = '\0';
}

static int tun_read_packet(uint8_t *buffer, size_t size)
{
    ssize_t len;

    if (tun_fd < 0 || buffer == NULL || size == 0U) {
        return -1;
    }

    len = read(tun_fd, buffer, size);
    if (len <= 0 || (size_t)len > size) {
        return -1;
    }

    return (int)len;
}

static int tun_write_packet(const uint8_t *buffer, size_t len)
{
    ssize_t written;

    if (tun_fd < 0 || buffer == NULL || len == 0U) {
        return -1;
    }

    written = write(tun_fd, buffer, len);
    if (written < 0 || (size_t)written != len) {
        return -1;
    }

    return 0;
}

static int ipv4_parse_endpoints(const uint8_t *packet, size_t len, uint32_t *src_ip, uint32_t *dst_ip)
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

static server_client_t *server_find_by_ip(uint32_t ip_be)
{
    size_t i;

    for (i = 0; i < CAMEX_MAX_CLIENTS; ++i) {
        if (server_clients[i].active && server_clients[i].ip_be == ip_be) {
            return &server_clients[i];
        }
    }

    return NULL;
}

static server_client_t *server_find_by_addr(const struct sockaddr_in *addr)
{
    size_t i;

    if (addr == NULL) {
        return NULL;
    }

    for (i = 0; i < CAMEX_MAX_CLIENTS; ++i) {
        if (server_clients[i].active && sockaddr_equal(&server_clients[i].addr, addr)) {
            return &server_clients[i];
        }
    }

    return NULL;
}

static server_client_t *server_alloc_client(void)
{
    size_t i;
    size_t oldest = 0;
    time_t oldest_seen = 0;
    int oldest_found = 0;

    for (i = 0; i < CAMEX_MAX_CLIENTS; ++i) {
        if (!server_clients[i].active) {
            memset(&server_clients[i], 0, sizeof(server_clients[i]));
            (void)get_random_bytes(server_clients[i].send_nonce_prefix, sizeof(server_clients[i].send_nonce_prefix));
            server_clients[i].active = 1U;
            return &server_clients[i];
        }

        if (!oldest_found || server_clients[i].last_seen < oldest_seen) {
            oldest_found = 1;
            oldest = i;
            oldest_seen = server_clients[i].last_seen;
        }
    }

    memset(&server_clients[oldest], 0, sizeof(server_clients[oldest]));
    (void)get_random_bytes(server_clients[oldest].send_nonce_prefix, sizeof(server_clients[oldest].send_nonce_prefix));
    server_clients[oldest].active = 1U;
    return &server_clients[oldest];
}

static server_client_t *server_upsert_client(uint32_t ip_be, const struct sockaddr_in *addr)
{
    server_client_t *entry = NULL;

    if (addr != NULL) {
        entry = server_find_by_addr(addr);
    }
    if (entry == NULL) {
        entry = server_find_by_ip(ip_be);
    }
    if (entry == NULL) {
        entry = server_alloc_client();
    }
    if (entry == NULL) {
        return NULL;
    }

    entry->ip_be = ip_be;
    if (addr != NULL) {
        entry->addr = *addr;
    }
    entry->last_seen = g_now;
    return entry;
}

static void server_expire_clients(void)
{
    static time_t last_expire_check = 0;
    size_t i;
    time_t now = (g_now != 0) ? g_now : time(NULL);

    if (now == last_expire_check) {
        return;
    }
    last_expire_check = now;

    for (i = 0; i < CAMEX_MAX_CLIENTS; ++i) {
        if (server_clients[i].active && difftime(now, server_clients[i].last_seen) > (double)CAMEX_CLIENT_TIMEOUT) {
            char ipbuf[INET_ADDRSTRLEN];
            struct in_addr addr;

            addr.s_addr = server_clients[i].ip_be;
            inet_ntop(AF_INET, &addr, ipbuf, sizeof(ipbuf));
            log_message(LOG_INFO, "Expired client %s", ipbuf);
            memset(&server_clients[i], 0, sizeof(server_clients[i]));
        }
    }
}

static void server_reset_replay(server_client_t *entry)
{
    if (entry == NULL) {
        return;
    }

    entry->recv_seq_max = 0;
    entry->recv_window = 0;
}

static int server_send_config_response(const struct sockaddr_in *from, server_client_t *entry, const char *client_id)
{
    char message[CAMEX_CONTROL_MAX];
    uint8_t packet[CAMEX_HDR_LEN + CAMEX_CONTROL_MAX + 16U];
    size_t packet_len = 0U;
    camex_profile_t profile;

    if (from == NULL) {
        return -1;
    }

    if (server_db_lookup_profile(client_id, &profile) != 0) {
        log_message(LOG_ERR, "Missing config for client %s", client_id != NULL ? client_id : "(null)");
        return -1;
    }

    if (profile.local_cidr[0] == '\0' || profile.gateway_ip[0] == '\0') {
        log_message(LOG_ERR, "Incomplete config for client %s", client_id != NULL ? client_id : "(null)");
        return -1;
    }

    if (build_config_message(client_id, &profile, message, sizeof(message)) != 0) {
        return -1;
    }

    if (current_config.encrypt) {
        if (entry == NULL) {
            entry = server_upsert_client(0U, from);
        }
        if (entry == NULL) {
            return -1;
        }
        if (crypto_encrypt_packet(CAMEX_PACKET_CONFIG, entry->send_seq++, entry->send_nonce_prefix, is_zero_key(entry->psk_key) ? NULL : entry->psk_key, (const uint8_t *)message, strlen(message), packet, sizeof(packet), &packet_len) != 0) {
            return -1;
        }
        return send_udp_payload(udp_socket, &entry->addr, packet, packet_len);
    }

    return send_udp_text(udp_socket, from, message);
}

static int server_handle_plain_register(const uint8_t *buffer, size_t len, const struct sockaddr_in *from, const uint8_t *used_key)
{
    char message[CAMEX_CONTROL_MAX];
    char local_ip[16];
    char client_id[CAMEX_CLIENT_ID_LEN];
    uint8_t auto_request = 0U;
    server_client_t *entry;
    struct in_addr addr;

    if (buffer == NULL || from == NULL || len == 0U) {
        return -1;
    }

    if (len >= sizeof(message)) {
        len = sizeof(message) - 1U;
    }

    memcpy(message, buffer, len);
    message[len] = '\0';
    local_ip[0] = '\0';
    client_id[0] = '\0';

    if (parse_register_message(message, client_id, sizeof(client_id), local_ip, sizeof(local_ip), &auto_request) != 0) {
        return -1;
    }

    /* Rate-limit: reject repeated REGISTER from same addr within 5 seconds */
    {
        server_client_t *existing = server_find_by_addr(from);
        if (existing != NULL && existing->last_register_time != 0 &&
            difftime(g_now, existing->last_register_time) < 5.0) {
            return 0;
        }
    }

    if (auto_request) {
        entry = server_upsert_client(0U, from);
        if (entry == NULL) {
            log_message(LOG_WARNING, "No free slots for client %s", client_id);
            return -1;
        }
        snprintf(entry->client_id, sizeof(entry->client_id), "%s", client_id);
        server_reset_replay(entry);
        entry->last_register_time = g_now;
        /* Store the key that authenticated this client for future packets. */
        if (used_key != NULL) {
            memcpy(entry->psk_key, used_key, 32);
        }
        if (server_send_config_response(from, entry, client_id) != 0) {
            return -1;
        }
        log_message(LOG_INFO, "Registered auto client %s", client_id);
        return 0;
    }

    if (inet_pton(AF_INET, local_ip, &addr) != 1) {
        log_message(LOG_WARNING, "Rejected invalid client IP: %s", local_ip);
        return -1;
    }

    entry = server_upsert_client(addr.s_addr, from);
    if (entry == NULL) {
        log_message(LOG_WARNING, "No free slots for client %s", local_ip);
        return -1;
    }

    server_reset_replay(entry);
    entry->last_register_time = g_now;
    /* Store the key that authenticated this client for future packets. */
    if (used_key != NULL) {
        memcpy(entry->psk_key, used_key, 32);
    }
    log_message(LOG_INFO, "Registered client %s", local_ip);
    return 0;
}

static int server_handle_register_packet(const uint8_t *plain, size_t plain_len, const struct sockaddr_in *from, uint64_t seq, const uint8_t *used_key)
{
    (void)seq;
    return server_handle_plain_register(plain, plain_len, from, used_key);
}

/*
 * Try every keystore entry to decrypt a packet.
 * Returns pointer to the matching psk_key on success, NULL on failure.
 * This is O(N) and is only called for unknown sources or PSK rotation.
 */
static const uint8_t *server_try_all_keys(
    const uint8_t *buffer, size_t len,
    uint8_t *type, uint8_t *plain, size_t plain_size,
    size_t *plain_len, uint64_t *seq)
{
    size_t i;

    for (i = 0U; i < server_keystore_count; ++i) {
        if (crypto_decrypt_packet(buffer, len, type, plain, plain_size, plain_len, seq, server_keystore[i].psk_key) == 0) {
            return server_keystore[i].psk_key;
        }
    }
    return NULL;
}

static int server_forward_packet(const uint8_t *packet, size_t len, uint32_t src_ip_be, uint32_t dst_ip_be)
{
    server_client_t *dst;
    static uint8_t encrypted[TUN_PACKET_MAX + CAMEX_HDR_LEN + 16U];
    size_t encrypted_len = 0;

    (void)src_ip_be;

    dst = server_find_by_ip(dst_ip_be);
    if (dst == NULL) {
        /* Packet destined for the server itself — deliver via local TUN */
        if (tun_fd >= 0) {
            return tun_write_packet(packet, len);
        }
        return -1;
    }

    dst->last_seen = (g_now != 0) ? g_now : time(NULL);

    if (current_config.encrypt) {
        if (crypto_encrypt_packet(CAMEX_PACKET_DATA, dst->send_seq++, dst->send_nonce_prefix, is_zero_key(dst->psk_key) ? NULL : dst->psk_key, packet, len, encrypted, sizeof(encrypted), &encrypted_len) != 0) {
            return -1;
        }
        return send_udp_payload(udp_socket, &dst->addr, encrypted, encrypted_len);
    }

    return send_udp_payload(udp_socket, &dst->addr, packet, len);
}

static int server_handle_packet(const uint8_t *buffer, size_t len, const struct sockaddr_in *from)
{
    static uint8_t plain[TUN_PACKET_MAX];
    uint8_t type = 0U;
    size_t plain_len = 0U;
    uint64_t seq = 0;
    uint32_t src_ip_be = 0;
    uint32_t dst_ip_be = 0;
    server_client_t *src;
    server_client_t *decrypt_src = NULL; /* entry whose key decrypted this packet */
    const uint8_t *used_key = NULL;

    if (buffer == NULL || from == NULL || len == 0U) {
        return -1;
    }

    if (current_config.encrypt) {
        src = server_find_by_addr(from);

        if (src != NULL && !is_zero_key(src->psk_key)) {
            /*
             * Fast path: known client with stored key — O(1) decrypt.
             * On failure (e.g., PSK rotation), fall through to try-all-keys.
             */
            if (crypto_decrypt_packet(buffer, len, &type, plain, sizeof(plain), &plain_len, &seq, src->psk_key) == 0) {
                used_key = src->psk_key;
                decrypt_src = src;
            }
        }

        if (used_key == NULL) {
            /* Slow path: unknown client or stored key failed — try all keystore entries. */
            used_key = server_try_all_keys(buffer, len, &type, plain, sizeof(plain), &plain_len, &seq);
            if (used_key == NULL) {
                return -1;
            }
            decrypt_src = server_find_by_addr(from); /* may be NULL for brand-new clients */
        }

        if (type == CAMEX_PACKET_REGISTER) {
            return server_handle_register_packet(plain, plain_len, from, seq, used_key);
        }

        if (type == CAMEX_PACKET_CONFIG) {
            return 0;
        }

        if (type != CAMEX_PACKET_DATA) {
            return -1;
        }

        buffer = plain;
        len = plain_len;
    }

    if (!current_config.encrypt && len >= 4U && memcmp(buffer, CAMEX_MAGIC, 4) == 0 && server_handle_plain_register(buffer, len, from, NULL) == 0) {
        return 0;
    }

    if (ipv4_parse_endpoints(buffer, len, &src_ip_be, &dst_ip_be) != 0) {
        return -1;
    }

    src = server_upsert_client(src_ip_be, from);
    if (src == NULL) {
        return -1;
    }

    if (current_config.encrypt) {
        /*
         * Replay check MUST use the entry whose key decrypted the packet.
         * server_upsert_client() above may have returned a different slot
         * (looked up by src IP from the packet body, which is attacker-controlled).
         * Using 'src' here instead of 'decrypt_src' would allow replay attacks.
         */
        server_client_t *check_entry = (decrypt_src != NULL) ? decrypt_src : src;
        if (replay_check(&check_entry->recv_seq_max, &check_entry->recv_window, seq) != 0) {
            return -1;
        }
    }

    return server_forward_packet(buffer, len, src_ip_be, dst_ip_be);
}

static int client_send_register(void)
{
    uint8_t packet[CAMEX_HDR_LEN + 16U + CAMEX_CONTROL_MAX];
    char message[CAMEX_CONTROL_MAX];
    size_t packet_len = 0;
    size_t message_len;

    if (build_register_message(&current_config, message, sizeof(message)) != 0) {
        return -1;
    }

    message_len = strlen(message);
    if (current_config.encrypt) {
        if (crypto_encrypt_packet(CAMEX_PACKET_REGISTER, client_state.send_seq++, client_state.send_nonce_prefix, NULL, (const uint8_t *)message, message_len, packet, sizeof(packet), &packet_len) != 0) {
            return -1;
        }
        if (send_udp_payload(udp_socket, NULL, packet, packet_len) != 0) {
            return -1;
        }
    } else {
        if (send_udp_text(udp_socket, NULL, message) != 0) {
            return -1;
        }
    }

    client_state.last_register = g_now;
    client_state.registered = 1U;
    return 0;
}

static int client_handle_udp_packet(const uint8_t *buffer, size_t len)
{
    static uint8_t plain[TUN_PACKET_MAX];
    uint8_t type = 0U;
    size_t plain_len = 0U;
    uint64_t seq = 0;

    if (buffer == NULL || len == 0U) {
        return -1;
    }

    if (current_config.encrypt) {
        if (crypto_decrypt_packet(buffer, len, &type, plain, sizeof(plain), &plain_len, &seq, NULL) != 0) {
            return -1;
        }

        if (type == CAMEX_PACKET_CONFIG && current_config.auto_config) {
            char message[CAMEX_CONTROL_MAX];
            int rc;

            if (replay_check(&client_state.recv_seq_max, &client_state.recv_window, seq) != 0) {
                return -1;
            }
            if (plain_len >= sizeof(message)) {
                return -1;
            }
            memcpy(message, plain, plain_len);
            message[plain_len] = '\0';
            rc = parse_config_message(message, &current_config);
            if (rc == 0) {
                client_state.config_received = 1U;
                log_message(LOG_INFO,
                            "Received config from server:"
                            " CIDR=%s GW=%s MTU=%d routes=%u",
                            current_config.local_cidr,
                            current_config.gateway_ip,
                            current_config.mtu,
                            (unsigned)current_config.route_count);
            }
            return rc;
        }

        if (type != CAMEX_PACKET_DATA) {
            return 0;
        }

        if (replay_check(&client_state.recv_seq_max, &client_state.recv_window, seq) != 0) {
            return -1;
        }

        buffer = plain;
        len = plain_len;
    } else if (current_config.auto_config && len >= 4U && memcmp(buffer, CAMEX_MAGIC, 4) == 0) {
        char message[CAMEX_CONTROL_MAX];
        int rc;

        if (len >= sizeof(message)) {
            return -1;
        }
        memcpy(message, buffer, len);
        message[len] = '\0';
        rc = parse_config_message(message, &current_config);
        if (rc == 0) {
            client_state.config_received = 1U;
            log_message(LOG_INFO,
                        "Received config from server:"
                        " CIDR=%s GW=%s MTU=%d routes=%u",
                        current_config.local_cidr,
                        current_config.gateway_ip,
                        current_config.mtu,
                        (unsigned)current_config.route_count);
        }
        return rc;
    }

    return tun_write_packet(buffer, len);
}

static int client_wait_for_config(void)
{
    uint8_t buffer[CAMEX_CONTROL_MAX];
    fd_set readset;
    struct timeval tv;
    int ready;
    unsigned int probe = 0; /* total 1-second select cycles */

    log_message(LOG_INFO, "Waiting for auto-config from server...");

    for (;;) {
        /* Exit cleanly on SIGINT/SIGTERM */
        if (!running) {
            return -1;
        }

        FD_ZERO(&readset);
        FD_SET(udp_socket, &readset);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        ready = select(udp_socket + 1, &readset, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            print_errno_message(LOG_ERR, "select");
            return -1;
        }

        if (ready == 0 || !FD_ISSET(udp_socket, &readset)) {
            ++probe;
            /* Re-send REGISTER every second; log every 10 seconds */
            if (probe % 10 == 0) {
                log_message(LOG_INFO, "Still waiting for server config (%us)...", probe);
            }
            if (client_send_register() != 0) {
                return -1;
            }
            continue;
        }

        ready = recv(udp_socket, buffer, sizeof(buffer), 0);
        if (ready <= 0) {
            continue;
        }
        if (client_handle_udp_packet(buffer, (size_t)ready) == 0 && current_config.local_cidr[0] != '\0' && current_config.gateway_ip[0] != '\0') {
            return 0;
        }
    }
}

static int client_handle_tun_packet(const uint8_t *buffer, size_t len)
{
    uint8_t packet[TUN_PACKET_MAX + CAMEX_HDR_LEN + 16U];
    size_t packet_len = 0;

    if (buffer == NULL || len == 0U || len > TUN_PACKET_MAX) {
        return -1;
    }

    if (current_config.encrypt) {
        if (crypto_encrypt_packet(CAMEX_PACKET_DATA, client_state.send_seq++, client_state.send_nonce_prefix, NULL, buffer, len, packet, sizeof(packet), &packet_len) != 0) {
            return -1;
        }
        return send_udp_payload(udp_socket, NULL, packet, packet_len);
    }

    return send_udp_payload(udp_socket, NULL, buffer, len);
}

/*
 * Re-resolve DNS, recreate UDP socket and re-send REGISTER.
 * Works for both manual and auto-config modes.
 */
static void client_reconnect(void)
{
    if (udp_socket >= 0) {
        close(udp_socket);
        udp_socket = -1;
    }

    log_message(LOG_INFO, "Reconnecting to server %s:%d...",
                current_config.server_host, current_config.port);

    if (client_socket_create(current_config.server_host, current_config.port) != 0) {
        log_message(LOG_WARNING, "Reconnect failed; will retry in %ds",
                    CAMEX_RECONNECT_INTERVAL);
        client_reconnect_at = g_now + CAMEX_RECONNECT_INTERVAL;
        client_state.last_recv = 0; /* keep at 0 until we actually reach the server */
        return;
    }

    client_state.registered = 0;
    client_state.last_register = 0;
    client_state.config_received = 0; /* will re-request CONFIG from server */
    client_state.last_recv = g_now; /* socket is up — seed the silence timer */
    client_reconnect_at = 0;

    if (client_send_register() != 0) {
        log_message(LOG_WARNING, "Failed to send register after reconnect");
    } else {
        log_message(LOG_NOTICE, "Connection to server %s:%d restored",
                    current_config.server_host, current_config.port);
        client_link_up = 1U;
        if (current_config.auto_config) {
            log_message(LOG_INFO, "Auto-config mode: waiting for server config response");
        }
    }
}

static void client_tick(void)
{
    /* Handle pending reconnect: socket gone or previous attempt failed */
    if (udp_socket < 0 || client_reconnect_at > 0) {
        if (client_reconnect_at == 0 ||
            difftime(g_now, client_reconnect_at) >= 0.0) {
            client_reconnect();
        }
        return;
    }

    /* Detect server silence.
     * In auto_config mode the server responds to every REGISTER with CONFIG,
     * so silence means the server is unreachable — trigger reconnect.
     * In manual mode the server never sends unsolicited data; rely on
     * REGISTER send errors to detect a broken path instead. */
    if (current_config.auto_config &&
        client_state.last_recv > 0 &&
        difftime(g_now, client_state.last_recv) >= (double)CAMEX_SERVER_TIMEOUT) {
        log_message(LOG_ERR,
                    "Connection lost: no data from server %s:%d for %ds",
                    current_config.server_host, current_config.port,
                    CAMEX_SERVER_TIMEOUT);
        client_link_up = 0U;
        client_state.last_recv = 0; /* prevent repeated trigger before reconnect */
        client_reconnect();
        return;
    }

    if (!client_state.registered ||
        difftime(g_now, client_state.last_register) >= (double)CAMEX_REGISTER_INTERVAL) {
        if (client_send_register() != 0) {
            log_message(LOG_ERR, "Failed to send REGISTER to server %s:%d — forcing reconnect",
                        current_config.server_host, current_config.port);
            close(udp_socket);
            udp_socket = -1;
            client_reconnect_at = 0;
        }
    }
}

static int server_socket_create(const char *bind_ip, int port)
{
    struct sockaddr_in addr;
    int reuse = 1;

    udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket < 0) {
        print_errno_message(LOG_ERR, "socket");
        return -1;
    }

    if (setsockopt(udp_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        print_errno_message(LOG_WARNING, "setsockopt(SO_REUSEADDR)");
    }

    if (current_config.bind_dev[0] != '\0') {
        if (setsockopt(udp_socket, SOL_SOCKET, SO_BINDTODEVICE,
                       current_config.bind_dev, strlen(current_config.bind_dev) + 1U) < 0) {
            print_errno_message(LOG_WARNING, "setsockopt(SO_BINDTODEVICE)");
        }
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind_ip != NULL && *bind_ip != '\0' && inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) {
        log_message(LOG_ERR, "Invalid bind IP: %s", bind_ip);
        close(udp_socket);
        udp_socket = -1;
        return -1;
    }

    if (bind(udp_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        print_errno_message(LOG_ERR, "bind");
        close(udp_socket);
        udp_socket = -1;
        return -1;
    }

    if (tune_udp_socket(udp_socket) != 0) {
        print_errno_message(LOG_ERR, "fcntl(O_NONBLOCK)");
        close(udp_socket);
        udp_socket = -1;
        return -1;
    }

    return 0;
}

static int client_socket_create(const char *host, int port)
{
    if (resolve_udp_endpoint(host, port, &server_addr, "server") != 0) {
        return -1;
    }

    udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket < 0) {
        print_errno_message(LOG_ERR, "socket");
        return -1;
    }

    if (current_config.bind_dev[0] != '\0') {
        if (setsockopt(udp_socket, SOL_SOCKET, SO_BINDTODEVICE,
                       current_config.bind_dev, strlen(current_config.bind_dev) + 1U) < 0) {
            print_errno_message(LOG_WARNING, "setsockopt(SO_BINDTODEVICE)");
        }
    }

    if (connect(udp_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        print_errno_message(LOG_ERR, "connect");
        close(udp_socket);
        udp_socket = -1;
        return -1;
    }

    if (tune_udp_socket(udp_socket) != 0) {
        print_errno_message(LOG_ERR, "fcntl(O_NONBLOCK)");
        close(udp_socket);
        udp_socket = -1;
        return -1;
    }

    return 0;
}

static int validate_config(camex_config_t *config)
{
    if (config == NULL) {
        return -1;
    }

    if (config->mtu < 576 || config->mtu > 9000) {
        log_message(LOG_ERR, "MTU must be in the range 576..9000");
        return -1;
    }

    if (config->encrypt && config->psk[0] == '\0') {
        log_message(LOG_ERR, "Encryption requested but PSK is empty");
        return -1;
    }

    switch (config->mode) {
    case CAMEX_MODE_CLIENT:
        if (config->server_host[0] == '\0' || config->port <= 0) {
            log_message(LOG_ERR, "Server host and port must be specified");
            return -1;
        }

        if (config->auto_config) {
            if (config->client_id[0] == '\0' && derive_client_id(config->client_id, sizeof(config->client_id)) != 0) {
                log_message(LOG_ERR, "Client name must be set with -n in auto mode when it cannot be derived from the default gateway interface");
                return -1;
            }
        } else {
            char local_ip[16];
            char local_netmask[16];
            size_t i;

            if (config->local_cidr[0] == '\0') {
                log_message(LOG_ERR, "Local CIDR must be specified");
                return -1;
            }
            if (config->gateway_ip[0] == '\0') {
                log_message(LOG_ERR, "Gateway IP must be specified");
                return -1;
            }
            if (parse_local_cidr(config->local_cidr, local_ip, sizeof(local_ip), local_netmask, sizeof(local_netmask)) != 0) {
                return -1;
            }
            if (validate_ipv4(config->gateway_ip, "gateway IP") != 0) {
                return -1;
            }

            for (i = 0; i < config->route_count; ++i) {
                if (validate_route_cidr(config->route_cidrs[i]) != 0) {
                    return -1;
                }
            }
        }

        break;
    case CAMEX_MODE_SERVER:
        if (config->auto_config) {
            log_message(LOG_ERR, "Auto mode is only available for client mode");
            return -1;
        }
        /* Port may come from the config file — checked later in camex_init. */
        break;
    default:
        log_message(LOG_ERR, "Unsupported mode");
        return -1;
    }

    return 0;
}

static int parse_arguments(int argc, char **argv, camex_config_t *config)
{
    static const struct option long_options[] = {
        { "mode", required_argument, NULL, 'M' },
        { "bind-ip", required_argument, NULL, 'b' },
        { "auto", no_argument, NULL, 'a' },
        { "name", required_argument, NULL, 'n' },
        { "config", required_argument, NULL, 'f' },
        { "local-cidr", required_argument, NULL, 'l' },
        { "gateway-ip", required_argument, NULL, 'g' },
        { "port", required_argument, NULL, 'p' },
        { "server-host", required_argument, NULL, 's' },
        { "route-cidr", required_argument, NULL, 'c' },
        { "mtu", required_argument, NULL, 't' },
        { "psk", required_argument, NULL, 'k' },
        { "encrypt", no_argument, NULL, 'e' },
        { "version", no_argument, NULL, 'v' },
        { "help", no_argument, NULL, 'h' },
        { "pid-file", required_argument, NULL, 'P' },
        { "bind-dev", required_argument, NULL, 'd' },
        { "tun-dev",  required_argument, NULL, 'T' },
        { NULL, 0, NULL, 0 }
    };
    int opt;
    int option_index = 0;

    opterr = 0;
    while ((opt = getopt_long(argc, argv, "M:b:an:f:l:g:p:s:c:t:k:evhP:d:T:", long_options, &option_index)) != -1) {
        switch (opt) {
        case 'M':
            if (parse_mode(optarg, &config->mode) != 0) {
                log_message(LOG_ERR, "Invalid mode: %s", optarg);
                return -1;
            }
            break;
        case 'b':
            if (copy_option(config->bind_ip, sizeof(config->bind_ip), optarg, "bind IP") != 0) {
                return -1;
            }
            break;
        case 'a':
            config->auto_config = 1U;
            break;
        case 'n':
            if (copy_option(config->client_id, sizeof(config->client_id), optarg, "client ID") != 0) {
                return -1;
            }
            break;
        case 'f':
            if (copy_option(config->config_path, sizeof(config->config_path), optarg, "config path") != 0) {
                return -1;
            }
            break;
        case 'l':
            if (copy_option(config->local_cidr, sizeof(config->local_cidr), optarg, "local CIDR") != 0) {
                return -1;
            }
            break;
        case 'g':
            if (copy_option(config->gateway_ip, sizeof(config->gateway_ip), optarg, "gateway IP") != 0) {
                return -1;
            }
            break;
        case 'p':
            if (parse_port(optarg, &config->port) != 0) {
                log_message(LOG_ERR, "Invalid port: %s", optarg);
                return -1;
            }
            break;
        case 's':
            if (copy_option(config->server_host, sizeof(config->server_host), optarg, "server host") != 0) {
                return -1;
            }
            break;
        case 'c':
            if (append_route_option(config->route_cidrs, &config->route_count, optarg, "route CIDR") != 0) {
                return -1;
            }
            break;
        case 't':
            if (parse_mtu(optarg, &config->mtu) != 0) {
                log_message(LOG_ERR, "Invalid MTU: %s", optarg);
                return -1;
            }
            break;
        case 'k':
            if (copy_option(config->psk, sizeof(config->psk), optarg, "PSK") != 0) {
                return -1;
            }
            break;
        case 'e':
            config->encrypt = 1U;
            break;
        case 'v':
            print_version();
            return 1;
        case 'h':
            print_usage(argv[0]);
            return 1;
        case 'P':
            if (copy_option(config->pid_file, sizeof(config->pid_file), optarg, "PID file") != 0) {
                return -1;
            }
            break;
        case 'd':
            if (copy_option(config->bind_dev, sizeof(config->bind_dev), optarg, "bind device") != 0) {
                return -1;
            }
            break;
        case 'T':
            if (copy_option(config->tun_dev, sizeof(config->tun_dev), optarg, "TUN device") != 0) {
                return -1;
            }
            break;
        case '?':
        default:
            log_message(LOG_ERR, "Unknown option: %s", argv[optind - 1]);
            return -1;
        }
    }

    return 0;
}

static void print_usage(const char *progname)
{
    printf("camex %s — minimal dependency-free UDP/TUN tunnel for embedded Linux\n\n", CAMEX_VERSION);

    printf("Features:\n");
    printf("  * Client/server architecture over UDP\n");
    printf("  * ChaCha20-Poly1305 authenticated encryption (optional)\n");
    printf("  * Auto-config: server pushes IP/route/MTU to the client\n");
    printf("  * Per-client pre-shared keys alongside a global PSK\n");
    printf("  * Replay protection (64-bit sliding window)\n");
    printf("  * Works with standard tun.ko (/dev/net/tun) or camex.ko (/dev/camex)\n");
    printf("  * Cross-compilable, no external dependencies\n");
    printf("  * Packet memory locked (mlockall), core dumps disabled\n\n");

    printf("Usage:\n");
    printf("  %s --mode client --server-host <addr> --port <port> [options]\n", progname);
    printf("  %s --mode client --auto --server-host <addr> --port <port> [options]\n", progname);
    printf("  %s --mode server --port <port> [options]\n\n", progname);

    printf("Mode selection:\n");
    printf("  -M, --mode <mode>         Operation mode: client or server (required)\n\n");

    printf("Modes:\n");
    printf("  client       Creates a TUN device and connects outbound to the server\n");
    printf("  server       Listens on UDP and relays packets between clients\n\n");

    printf("Client options:\n");
    printf("  -a, --auto                Fetch tunnel parameters from the server\n");
    printf("  -n, --name <id>           Client ID used in auto mode\n");
    printf("  -l, --local-cidr <cidr>   Local tunnel address in CIDR form\n");
    printf("  -g, --gateway-ip <addr>   Gateway inside the tunnel\n");
    printf("  -s, --server-host <addr>  Tunnel server host or address\n");
    printf("  -p, --port <port>         Tunnel server UDP port\n");
    printf("  -c, --route-cidr <cidr>   Route to install through the tunnel (repeatable)\n");
    printf("  -T, --tun-dev <path>      TUN device path (default: auto-detect /dev/net/tun then /dev/camex)\n\n");

    printf("Server options:\n");
    printf("  -f, --config <path>       Server config file path (default: %s)\n", CAMEX_DEFAULT_CONFIG_PATH);
    printf("  -b, --bind-ip <addr>      Optional bind address (default: 0.0.0.0)\n");
    printf("  -p, --port <port>         UDP port to listen on\n");
    printf("  -d, --bind-dev <iface>    Bind socket to a specific network interface\n\n");

    printf("Common options:\n");
    printf("  -t, --mtu <size>          Tunnel MTU, 576–9000 (default: 1500)\n");
    printf("  -k, --psk <key>           Passphrase used to derive the session key\n");
    printf("  -e, --encrypt             Enable ChaCha20-Poly1305 transport encryption\n");
    printf("  -P, --pid-file <path>     Write PID to file on startup\n");
    printf("  -v, --version             Show program version and exit\n");
    printf("  -h, --help                Show this help message and exit\n\n");

    printf("Examples:\n");
    printf("  # Server\n");
    printf("  %s --mode server --port 7000 --config /etc/camex/camex.conf --encrypt --psk secret\n\n", progname);
    printf("  # Client (auto-config)\n");
    printf("  %s --mode client --auto --name 0203A104B5AE --server-host cloud.openipc.org --port 7000 --encrypt --psk secret\n\n", progname);
    printf("  # Client (manual)\n");
    printf("  %s --mode client --local-cidr 10.0.0.2/24 --gateway-ip 10.0.0.1 --server-host cloud.openipc.org --port 7000\n", progname);
}

static int validate_and_prepare_client(camex_config_t *config)
{
    char local_ip[16];
    char local_netmask[16];
    size_t i;

    if (parse_local_cidr(config->local_cidr, local_ip, sizeof(local_ip), local_netmask, sizeof(local_netmask)) != 0) {
        return -1;
    }

    snprintf(config->local_ip, sizeof(config->local_ip), "%s", local_ip);
    if (validate_ipv4(config->gateway_ip, "gateway IP") != 0) {
        return -1;
    }

    for (i = 0; i < config->route_count; ++i) {
        if (validate_route_cidr(config->route_cidrs[i]) != 0) {
            return -1;
        }
    }

    return 0;
}

static int append_control_field(char *buffer, size_t size, size_t *offset, const char *fmt, ...)
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

static int token_value(const char *token, const char *key, char *value, size_t value_size)
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

static int build_register_message(camex_config_t *config, char *buffer, size_t size)
{
    size_t offset = 0U;

    if (config == NULL || buffer == NULL || size == 0U) {
        return -1;
    }

    if (append_control_field(buffer, size, &offset, "%s REGISTER", CAMEX_MAGIC) != 0) {
        return -1;
    }

    if (config->auto_config && !client_state.config_received) {
        /* First registration or after reconnect: request full config from server. */
        if (append_control_field(buffer, size, &offset, " MODE=AUTO CLIENT_ID=%s", config->client_id) != 0) {
            return -1;
        }
    } else {
        /* Already configured (or manual mode): plain keepalive, server won't re-send CONFIG. */
        if (append_control_field(buffer, size, &offset, " MODE=MANUAL LOCAL_IP=%s", config->local_ip) != 0) {
            return -1;
        }
    }

    if (append_control_field(buffer, size, &offset, "\n") != 0) {
        return -1;
    }

    return 0;
}

static int parse_register_message(char *message, char *client_id, size_t client_id_size, char *local_ip, size_t local_ip_size, uint8_t *auto_request)
{
    char *saveptr = NULL;
    char *token;
    int saw_mode = 0;

    if (message == NULL || auto_request == NULL) {
        return -1;
    }

    token = strtok_r(message, " \t\r\n", &saveptr);
    if (token == NULL || strcmp(token, CAMEX_MAGIC) != 0) {
        return -1;
    }

    token = strtok_r(NULL, " \t\r\n", &saveptr);
    if (token == NULL || strcmp(token, "REGISTER") != 0) {
        return -1;
    }

    while ((token = strtok_r(NULL, " \t\r\n", &saveptr)) != NULL) {
        char value[CAMEX_CLIENT_TOKEN_LEN];

        if (token_value(token, "MODE", value, sizeof(value)) == 0) {
            saw_mode = 1;
            if (strcasecmp(value, "AUTO") == 0) {
                *auto_request = 1U;
            } else if (strcasecmp(value, "MANUAL") == 0) {
                *auto_request = 0U;
            } else {
                log_message(LOG_ERR, "Invalid register mode: %s", value);
                return -1;
            }
            continue;
        }

        if (token_value(token, "CLIENT_ID", value, sizeof(value)) == 0) {
            if (copy_option(client_id, client_id_size, value, "client ID") != 0) {
                return -1;
            }
            continue;
        }

        if (token_value(token, "LOCAL_IP", value, sizeof(value)) == 0) {
            if (copy_option(local_ip, local_ip_size, value, "local IP") != 0) {
                return -1;
            }
            continue;
        }

        if (!saw_mode && client_id != NULL && *client_id == '\0' && local_ip != NULL && *local_ip == '\0') {
            if (validate_ipv4(token, "client IP") != 0) {
                return -1;
            }
            if (copy_option(local_ip, local_ip_size, token, "local IP") != 0) {
                return -1;
            }
            *auto_request = 0U;
            continue;
        }

        log_message(LOG_ERR, "Unknown register token: %s", token);
        return -1;
    }

    if (*auto_request) {
        if (client_id == NULL || *client_id == '\0') {
            log_message(LOG_ERR, "Missing client ID in auto register request");
            return -1;
        }
    } else if (local_ip == NULL || *local_ip == '\0') {
        log_message(LOG_ERR, "Missing local IP in manual register request");
        return -1;
    }

    return 0;
}

static int build_config_message(const char *client_id, const camex_profile_t *profile, char *buffer, size_t size)
{
    size_t offset = 0U;
    size_t i;

    if (buffer == NULL || profile == NULL || size == 0U) {
        return -1;
    }

    if (append_control_field(buffer, size, &offset, "%s CONFIG", CAMEX_MAGIC) != 0) {
        return -1;
    }
    if (client_id != NULL && *client_id != '\0') {
        if (append_control_field(buffer, size, &offset, " CLIENT_ID=%s", client_id) != 0) {
            return -1;
        }
    }
    if (profile->mtu > 0) {
        if (append_control_field(buffer, size, &offset, " MTU=%d", profile->mtu) != 0) {
            return -1;
        }
    }
    if (profile->local_cidr[0] != '\0') {
        if (append_control_field(buffer, size, &offset, " LOCAL_CIDR=%s", profile->local_cidr) != 0) {
            return -1;
        }
    }
    if (profile->gateway_ip[0] != '\0') {
        if (append_control_field(buffer, size, &offset, " GATEWAY_IP=%s", profile->gateway_ip) != 0) {
            return -1;
        }
    }
    for (i = 0; i < profile->route_count; ++i) {
        if (append_control_field(buffer, size, &offset, " ROUTE_CIDR=%s", profile->route_cidrs[i]) != 0) {
            return -1;
        }
    }
    if (append_control_field(buffer, size, &offset, "\n") != 0) {
        return -1;
    }

    return 0;
}

static int apply_config_profile(camex_config_t *config, const camex_profile_t *profile)
{
    size_t i;

    if (config == NULL || profile == NULL) {
        return -1;
    }

    if (profile->local_cidr[0] == '\0' || profile->gateway_ip[0] == '\0') {
        return -1;
    }

    snprintf(config->local_cidr, sizeof(config->local_cidr), "%s", profile->local_cidr);
    snprintf(config->gateway_ip, sizeof(config->gateway_ip), "%s", profile->gateway_ip);
    config->route_count = 0U;
    for (i = 0; i < profile->route_count && i < CAMEX_MAX_ROUTES; ++i) {
        snprintf(config->route_cidrs[config->route_count], sizeof(config->route_cidrs[0]), "%s", profile->route_cidrs[i]);
        ++config->route_count;
    }
    if (profile->mtu > 0) {
        config->mtu = profile->mtu;
    }

    return 0;
}

static int parse_config_message(char *message, camex_config_t *config)
{
    char *saveptr = NULL;
    char *token;
    camex_profile_t profile;

    if (message == NULL || config == NULL) {
        return -1;
    }

    profile_reset(&profile);

    token = strtok_r(message, " \t\r\n", &saveptr);
    if (token == NULL || strcmp(token, CAMEX_MAGIC) != 0) {
        return -1;
    }

    token = strtok_r(NULL, " \t\r\n", &saveptr);
    if (token == NULL || strcmp(token, "CONFIG") != 0) {
        return -1;
    }

    while ((token = strtok_r(NULL, " \t\r\n", &saveptr)) != NULL) {
        char value[CAMEX_CLIENT_TOKEN_LEN];

        if (token_value(token, "CLIENT_ID", value, sizeof(value)) == 0) {
            if (copy_option(config->client_id, sizeof(config->client_id), value, "client ID") != 0) {
                return -1;
            }
            continue;
        }
        if (token_value(token, "LOCAL_CIDR", value, sizeof(value)) == 0) {
            if (copy_option(profile.local_cidr, sizeof(profile.local_cidr), value, "local CIDR") != 0) {
                return -1;
            }
            continue;
        }
        if (token_value(token, "GATEWAY_IP", value, sizeof(value)) == 0) {
            if (copy_option(profile.gateway_ip, sizeof(profile.gateway_ip), value, "gateway IP") != 0) {
                return -1;
            }
            continue;
        }
        if (token_value(token, "ROUTE_CIDR", value, sizeof(value)) == 0) {
            if (profile_append_route(&profile, value) != 0) {
                return -1;
            }
            continue;
        }
        if (token_value(token, "MTU", value, sizeof(value)) == 0) {
            if (parse_mtu(value, &profile.mtu) != 0) {
                log_message(LOG_ERR, "Invalid MTU in config response: %s", value);
                return -1;
            }
            continue;
        }

        log_message(LOG_ERR, "Unknown config token: %s", token);
        return -1;
    }

    if (apply_config_profile(config, &profile) != 0) {
        log_message(LOG_ERR, "Invalid config response");
        return -1;
    }

    return 0;
}

/*
 * Build the server keystore from the global PSK and all per-client PSKs
 * defined in the config file.  Called once after server_db_load_file().
 * On SIGHUP, server_db_load_file() + server_build_keystore() are called
 * again to pick up any config changes.
 *
 * Plaintext PSK strings in server_db are wiped after key derivation.
 */
static void server_build_keystore(void)
{
    size_t i;

    server_keystore_count = 0U;
    memset(server_keystore, 0, sizeof(server_keystore));

    /* Entry 0: global PSK derived from --psk / -p option */
    if (crypto_ctx.ready) {
        memcpy(server_keystore[0].psk_key, crypto_ctx.psk_key, 32);
        server_keystore_count = 1U;
    }

    /* Entries 1..N: per-client PSKs from [client X] sections */
    for (i = 0U; i < CAMEX_MAX_CLIENTS && server_keystore_count < (size_t)(CAMEX_MAX_CLIENTS + 1); ++i) {
        if (!server_db.clients[i].active) {
            continue;
        }
        if (server_db.clients[i].profile.psk[0] == '\0') {
            continue;
        }
        derive_psk_key(server_db.clients[i].profile.psk, server_keystore[server_keystore_count].psk_key);
        server_keystore_count++;
    }

    /* Wipe plaintext PSK strings from memory now that keys are derived */
    for (i = 0U; i < CAMEX_MAX_CLIENTS; ++i) {
        if (server_db.clients[i].profile.psk[0] != '\0') {
            crypto_wipe(server_db.clients[i].profile.psk, sizeof(server_db.clients[i].profile.psk));
        }
    }
    crypto_wipe(server_db.defaults.psk, sizeof(server_db.defaults.psk));

    log_message(LOG_INFO, "Server keystore: %zu PSK(s) loaded", server_keystore_count);
}

int camex_init(camex_config_t *config)
{
    char local_ip[16];
    char local_netmask[16];
    char endpoint[64];

    if (config == NULL) {
        log_message(LOG_ERR, "Configuration is missing");
        return -1;
    }

    memcpy(&current_config, config, sizeof(current_config));
    client_mode = (config->mode == CAMEX_MODE_CLIENT) ? 1U : 0U;
    server_mode = (config->mode == CAMEX_MODE_SERVER) ? 1U : 0U;
    memset(&client_state, 0, sizeof(client_state));
    memset(server_clients, 0, sizeof(server_clients));

    if (get_random_bytes(client_state.send_nonce_prefix, sizeof(client_state.send_nonce_prefix)) != 0) {
        log_message(LOG_WARNING, "Failed to generate nonce prefix; using zeros");
    }

    /*
     * For server mode: load config file first so that [server] globals
     * (port, encrypt, psk, local_cidr, etc.) are available before crypto_init.
     * CLI arguments take priority — only fill fields that are still unset.
     */
    if (server_mode) {
        if (server_db_load_file(current_config.config_path) != 0) {
            return -1;
        }

        {
            const camex_server_globals_t *g = &server_db.globals;

            if (current_config.port == 0 && g->port > 0) {
                current_config.port = g->port;
            }
            if (current_config.bind_ip[0] == '\0' && g->bind_ip[0] != '\0') {
                snprintf(current_config.bind_ip, sizeof(current_config.bind_ip), "%s", g->bind_ip);
            }
            if (current_config.local_cidr[0] == '\0' && g->local_cidr[0] != '\0') {
                snprintf(current_config.local_cidr, sizeof(current_config.local_cidr), "%s", g->local_cidr);
            }
            if (current_config.tun_dev[0] == '\0' && g->tun_dev[0] != '\0') {
                snprintf(current_config.tun_dev, sizeof(current_config.tun_dev), "%s", g->tun_dev);
            }
            if (current_config.bind_dev[0] == '\0' && g->bind_dev[0] != '\0') {
                snprintf(current_config.bind_dev, sizeof(current_config.bind_dev), "%s", g->bind_dev);
            }
            if (current_config.pid_file[0] == '\0' && g->pid_file[0] != '\0') {
                snprintf(current_config.pid_file, sizeof(current_config.pid_file), "%s", g->pid_file);
            }
            if (current_config.psk[0] == '\0' && g->psk[0] != '\0') {
                snprintf(current_config.psk, sizeof(current_config.psk), "%s", g->psk);
            }
            if (!current_config.encrypt && g->encrypt_set) {
                current_config.encrypt = g->encrypt;
            }
            if (current_config.mtu == 1500 && g->mtu > 0) {
                current_config.mtu = g->mtu;
            }
        }

        /* Validate port after merging config file values */
        if (current_config.port <= 0) {
            log_message(LOG_ERR, "Server port must be specified (use -p or port= in [server] section)");
            return -1;
        }
    }

    if (current_config.encrypt) {
        if (crypto_init(current_config.psk) != 0) {
            return -1;
        }
        crypto_wipe(current_config.psk, sizeof(current_config.psk));
    }

    if (client_mode) {
        if (!current_config.auto_config && validate_and_prepare_client(&current_config) != 0) {
            return -1;
        }

        {
            unsigned int dns_attempt = 0;
            while (running) {
                if (client_socket_create(current_config.server_host, current_config.port) == 0) {
                    break;
                }
                dns_attempt++;
                if (dns_attempt == 1 || dns_attempt % 12 == 0) {
                    log_message(LOG_WARNING, "Cannot reach server %s:%d, retrying every 5s...",
                                current_config.server_host, current_config.port);
                }
                {
                    int s;
                    for (s = 0; s < 5 && running; s++) {
                        usleep(1000000);
                    }
                }
            }
            if (!running) {
                return -1;
            }
        }

        /* Seed the server-silence timer so we don't reconnect immediately */
        client_state.last_recv = time(NULL);

        if (current_config.auto_config) {
            if (client_send_register() != 0) {
                log_message(LOG_WARNING, "Initial auto registration failed");
            }
            if (client_wait_for_config() != 0) {
                close(udp_socket);
                udp_socket = -1;
                return -1;
            }
        }

        if (parse_local_cidr(current_config.local_cidr, local_ip, sizeof(local_ip), local_netmask, sizeof(local_netmask)) != 0) {
            close(udp_socket);
            udp_socket = -1;
            return -1;
        }

        if (tun_create_device(local_ip, local_netmask, current_config.mtu) != 0) {
            close(udp_socket);
            udp_socket = -1;
            return -1;
        }

        if (!current_config.auto_config && client_send_register() != 0) {
            log_message(LOG_WARNING, "Initial client registration failed");
        }

        client_link_up = 1U;
        log_message(LOG_NOTICE, "Connection to server %s:%d established",
                    current_config.server_host, current_config.port);
        log_message(LOG_INFO, "Tunnel initialized");
        log_message(LOG_INFO, "  Mode: %s", mode_to_string(current_config.mode));
        log_message(LOG_INFO, "  Server: %s:%d", current_config.server_host, current_config.port);
        log_message(LOG_INFO, "  Local CIDR: %s", current_config.local_cidr);
        log_message(LOG_INFO, "  Tunnel device: %s (backend: %s)", tun_name,
                    (current_config.tun_dev[0] != '\0') ? current_config.tun_dev : "auto-detect");
        log_message(LOG_INFO, "  MTU: %d", current_config.mtu);
        log_message(LOG_INFO, "  Encryption: %s", current_config.encrypt ? "enabled" : "disabled");
        return 0;
    }

    /* server_db is already loaded above; just build keystore and continue */
    server_build_keystore();

    if (server_socket_create(current_config.bind_ip, current_config.port) != 0) {
        return -1;
    }

    snprintf(endpoint, sizeof(endpoint), "%s:%d",
             current_config.bind_ip[0] != '\0' ? current_config.bind_ip : "0.0.0.0",
             current_config.port);

    /* Create TUN interface if the server has a local CIDR configured */
    if (current_config.local_cidr[0] != '\0') {
        if (parse_local_cidr(current_config.local_cidr, local_ip, sizeof(local_ip),
                             local_netmask, sizeof(local_netmask)) != 0) {
            close(udp_socket);
            udp_socket = -1;
            return -1;
        }

        if (tun_create_device(local_ip, local_netmask, current_config.mtu) != 0) {
            close(udp_socket);
            udp_socket = -1;
            return -1;
        }
    }

    log_message(LOG_INFO, "Tunnel initialized");
    log_message(LOG_INFO, "  Mode: %s", mode_to_string(current_config.mode));
    log_message(LOG_INFO, "  Listen endpoint: %s", endpoint);
    if (current_config.local_cidr[0] != '\0') {
        log_message(LOG_INFO, "  Local CIDR: %s", current_config.local_cidr);
        log_message(LOG_INFO, "  Tunnel device: %s (backend: %s)", tun_name,
                    (current_config.tun_dev[0] != '\0') ? current_config.tun_dev : "auto-detect");
    }
    log_message(LOG_INFO, "  MTU: %d", current_config.mtu);
    log_message(LOG_INFO, "  Encryption: %s", current_config.encrypt ? "enabled" : "disabled");
    return 0;
}

static void handle_udp_packet(void)
{
    static uint8_t buffer[TUN_PACKET_MAX + CAMEX_HDR_LEN + 16U];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    int len;

    if (server_mode) {
        while (1) {
            fromlen = sizeof(from);
            len = recvfrom(udp_socket, buffer, sizeof(buffer), 0, (struct sockaddr *)&from, &fromlen);
            if (len > 0) {
                if (server_handle_packet(buffer, (size_t)len, &from) != 0) {
                    char peer[64];
                    sockaddr_to_string(&from, peer, sizeof(peer));
                    log_message(LOG_WARNING, "Dropped packet from %s", peer);
                }
                continue;
            }

            if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }

            break;
        }
        return;
    }

    while (1) {
        len = recv(udp_socket, buffer, sizeof(buffer), 0);
        if (len > 0) {
            client_state.last_recv = g_now; /* server is alive */
            if (client_handle_udp_packet(buffer, (size_t)len) != 0) {
                log_message(LOG_WARNING, "Dropped packet from server");
            }
            continue;
        }

        if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }

        break;
    }
}

/* Forward a packet read from the local TUN to the appropriate UDP client. */
static int server_handle_tun_packet(const uint8_t *buffer, size_t len)
{
    uint32_t src_ip_be = 0;
    uint32_t dst_ip_be = 0;

    if (ipv4_parse_endpoints(buffer, len, &src_ip_be, &dst_ip_be) != 0) {
        return -1;
    }

    return server_forward_packet(buffer, len, src_ip_be, dst_ip_be);
}

static void handle_tun_packet(void)
{
    static uint8_t buffer[TUN_PACKET_MAX];
    int len;

    while (1) {
        len = tun_read_packet(buffer, sizeof(buffer));
        if (len > 0) {
            int rc;
            if (server_mode) {
                rc = server_handle_tun_packet(buffer, (size_t)len);
            } else {
                rc = client_handle_tun_packet(buffer, (size_t)len);
            }
            if (rc != 0) {
                log_message(LOG_WARNING, "Dropped packet from tunnel device");
            }
            continue;
        }

        if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }

        if (len < 0) {
            print_errno_message(LOG_WARNING, "read(tun)");
        }
        break;
    }
}

static void signal_handler(int sig)
{
    (void)sig;
    running = 0;
}

static void sighup_handler(int sig)
{
    (void)sig;
    reload_config = 1;
}

void camex_run(void)
{
    fd_set readset;
    struct timeval tv;
    int maxfd;
    int ready;

    if (server_mode) {
        log_message(LOG_INFO, "Server started. Press Ctrl+C to stop.");
        while (running) {
            g_now = time(NULL);
            FD_ZERO(&readset);
            FD_SET(udp_socket, &readset);
            if (tun_fd >= 0) {
                FD_SET(tun_fd, &readset);
            }

            maxfd = udp_socket;
            if (tun_fd > maxfd) {
                maxfd = tun_fd;
            }

            tv.tv_sec = 1;
            tv.tv_usec = 0;

            ready = select(maxfd + 1, &readset, NULL, NULL, &tv);
            if (ready > 0) {
                if (FD_ISSET(udp_socket, &readset)) {
                    handle_udp_packet();
                }
                if (tun_fd >= 0 && FD_ISSET(tun_fd, &readset)) {
                    handle_tun_packet();
                }
            } else if (ready < 0 && errno != EINTR) {
                print_errno_message(LOG_ERR, "select");
            }

            server_expire_clients();
            if (reload_config) {
                reload_config = 0;
                log_message(LOG_INFO, "Reloading config: %s", current_config.config_path);
                (void)server_db_load_file(current_config.config_path);
                server_build_keystore();
            }
        }

        log_message(LOG_INFO, "Server stopped.");
        return;
    }

    log_message(LOG_INFO, "Client started. Press Ctrl+C to stop.");
    while (running) {
        g_now = time(NULL);

        /* Socket may be temporarily unavailable during reconnect */
        if (udp_socket < 0) {
            usleep(1000000);
            client_tick();
            continue;
        }

        FD_ZERO(&readset);
        FD_SET(udp_socket, &readset);
        if (tun_fd >= 0) {
            FD_SET(tun_fd, &readset);
        }

        tv.tv_sec = 1;
        tv.tv_usec = 0;

        maxfd = udp_socket;
        if (tun_fd > maxfd) {
            maxfd = tun_fd;
        }

        ready = select(maxfd + 1, &readset, NULL, NULL, &tv);
        if (ready > 0) {
            if (FD_ISSET(udp_socket, &readset)) {
                handle_udp_packet();
            }
            if (tun_fd >= 0 && FD_ISSET(tun_fd, &readset)) {
                handle_tun_packet();
            }
        } else if (ready < 0 && errno != EINTR) {
            print_errno_message(LOG_ERR, "select");
        }

        client_tick();
    }

    log_message(LOG_INFO, "Client stopped.");
}

void camex_stop(void)
{
    size_t i;

    running = 0;

    if (client_mode && current_config.gateway_ip[0] != '\0') {
        for (i = 0; i < current_config.route_count; ++i) {
            (void)camex_del_route(current_config.route_cidrs[i], current_config.gateway_ip);
        }
    }

    tun_close_device();

    if (udp_socket >= 0) {
        close(udp_socket);
        udp_socket = -1;
    }

    crypto_wipe(&crypto_ctx, sizeof(crypto_ctx));
    crypto_wipe(&client_state, sizeof(client_state));
    crypto_wipe(server_clients, sizeof(server_clients));
}

static void print_version(void)
{
    printf("camex %s (built %s %s)\n", CAMEX_VERSION, __DATE__, __TIME__);
}

int main(int argc, char *argv[])
{
    camex_config_t config;
    int parse_result;

    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    openlog("camex", LOG_PID, LOG_DAEMON);
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        log_message(LOG_ERR,
                    "SECURITY WARNING: mlockall failed (%s) — sensitive key material may be swapped to disk",
                    strerror(errno));
    }
    prctl(PR_SET_DUMPABLE, 0);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, sighup_handler);

    memset(&config, 0, sizeof(config));
    config.mode = CAMEX_MODE_CLIENT;
    config.mtu = 1500;
    snprintf(config.config_path, sizeof(config.config_path), "%s", CAMEX_DEFAULT_CONFIG_PATH);

    /* No arguments at all — show help and exit cleanly */
    if (argc == 1) {
        print_usage(argv[0]);
        closelog();
        return 0;
    }

    parse_result = parse_arguments(argc, argv, &config);
    if (parse_result != 0) {
        if (parse_result > 0) {
            closelog();
            return 0;
        }

        print_usage(argv[0]);
        closelog();
        return 1;
    }

    if (validate_config(&config) != 0) {
        print_usage(argv[0]);
        closelog();
        return 1;
    }

    if (camex_init(&config) != 0) {
        log_message(LOG_ERR, "Initialization failed");
        closelog();
        return 1;
    }

    if (config.pid_file[0] != '\0') {
        FILE *pf = fopen(config.pid_file, "w");
        if (pf != NULL) {
            fprintf(pf, "%d\n", (int)getpid());
            fclose(pf);
        } else {
            print_errno_message(LOG_WARNING, "Cannot write PID file");
        }
    }

    if (client_mode) {
        size_t i;

        for (i = 0; i < current_config.route_count; ++i) {
            if (camex_add_route(current_config.route_cidrs[i], current_config.gateway_ip) != 0) {
                log_message(LOG_WARNING, "Route setup failed for %s", current_config.route_cidrs[i]);
            }
        }
    }

    camex_run();
    camex_stop();
    if (config.pid_file[0] != '\0') {
        unlink(config.pid_file);
    }
    closelog();
    return 0;
}
