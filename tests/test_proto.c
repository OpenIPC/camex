/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * test_proto.c — unit tests for control message parsing
 *
 */

#include "camex.h"
#include "client.h"
#include "config.h"
#include "crypto.h"
#include "proto.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Globals normally provided by camex.c / client.c */
camex_config_t current_config;
volatile sig_atomic_t running = 1;
volatile sig_atomic_t reload_config = 0;
time_t g_now = 0;
uint8_t client_mode = 0;
uint8_t server_mode = 0;
client_state_t client_state;

static int failures = 0;

/* Parse msg into a fresh config; returns the parser result. */
static int parse(const char *msg, camex_config_t *cfg)
{
    char buf[CAMEX_CONTROL_MAX];

    memset(cfg, 0, sizeof(*cfg));
    snprintf(buf, sizeof(buf), "%s", msg);
    return proto_parse_config(buf, cfg);
}

static void expect(const char *msg, int want_ok, const char *what)
{
    camex_config_t cfg;
    int rc = parse(msg, &cfg);

    if ((rc == 0) != want_ok) {
        fprintf(stderr, "FAIL: %s: expected %s, got rc=%d\n  %s\n",
                what, want_ok ? "accept" : "reject", rc, msg);
        ++failures;
    }
}

int main(void)
{
    camex_config_t cfg;

    /* Valid message is accepted and applied */
    if (parse("CAMEX CONFIG VER=1 CLIENT_ID=cam1 MTU=1400 "
              "LOCAL_CIDR=10.200.0.5/24 GATEWAY_IP=10.200.0.1 "
              "ROUTE_CIDR=192.168.1.0/24", &cfg) != 0 ||
        strcmp(cfg.local_cidr, "10.200.0.5/24") != 0 ||
        strcmp(cfg.gateway_ip, "10.200.0.1") != 0 ||
        cfg.mtu != 1400 || cfg.route_count != 1) {
        fprintf(stderr, "FAIL: valid CONFIG not applied correctly\n");
        ++failures;
    }

    /* #12: shell metacharacters must never get through */
    expect("CAMEX CONFIG LOCAL_CIDR=10.200.0.5/24 GATEWAY_IP=1;reboot",
           0, "gateway with ';'");
    expect("CAMEX CONFIG LOCAL_CIDR=10.200.0.5/24 GATEWAY_IP=$(id)",
           0, "gateway with $()");
    expect("CAMEX CONFIG LOCAL_CIDR=10.200.0.5/24 GATEWAY_IP=10.0.0.1|sh",
           0, "gateway with '|'");
    expect("CAMEX CONFIG LOCAL_CIDR=10.200.0.5/24 GATEWAY_IP=gw.example",
           0, "gateway that is not an IPv4 address");
    expect("CAMEX CONFIG LOCAL_CIDR=10.200.0.5/24;id GATEWAY_IP=10.200.0.1",
           0, "local CIDR with ';'");
    expect("CAMEX CONFIG LOCAL_CIDR=10.200.0.5/33 GATEWAY_IP=10.200.0.1",
           0, "local CIDR prefix out of range");
    expect("CAMEX CONFIG LOCAL_CIDR=10.200.0.5 GATEWAY_IP=10.200.0.1",
           0, "local CIDR without prefix");
    expect("CAMEX CONFIG LOCAL_CIDR=10.200.0.5/24 GATEWAY_IP=10.200.0.1 "
           "ROUTE_CIDR=1.2.3.0/24;id", 0, "route with ';'");
    expect("CAMEX CONFIG LOCAL_CIDR=10.200.0.5/24 GATEWAY_IP=10.200.0.1 MTU=99999",
           0, "MTU out of range");

    /* A CONFIG for another client ID is rejected */
    {
        char buf[CAMEX_CONTROL_MAX];

        memset(&cfg, 0, sizeof(cfg));
        snprintf(cfg.client_id, sizeof(cfg.client_id), "cam1");
        snprintf(buf, sizeof(buf), "%s",
                 "CAMEX CONFIG CLIENT_ID=other LOCAL_CIDR=10.200.0.5/24 "
                 "GATEWAY_IP=10.200.0.1");
        if (proto_parse_config(buf, &cfg) == 0 ||
            strcmp(cfg.client_id, "cam1") != 0 || cfg.local_cidr[0] != '\0') {
            fprintf(stderr, "FAIL: a CONFIG for another client was applied\n");
            ++failures;
        }
    }

    /* A rejected CONFIG leaves the client ID unchanged */
    {
        char buf[CAMEX_CONTROL_MAX];

        memset(&cfg, 0, sizeof(cfg));
        snprintf(cfg.client_id, sizeof(cfg.client_id), "cam1");
        snprintf(buf, sizeof(buf), "%s",
                 "CAMEX CONFIG CLIENT_ID=other LOCAL_CIDR=10.200.0.5/24 "
                 "GATEWAY_IP=1;reboot");
        if (proto_parse_config(buf, &cfg) == 0 ||
            strcmp(cfg.client_id, "cam1") != 0) {
            fprintf(stderr, "FAIL: rejected CONFIG changed client ID to %s\n",
                    cfg.client_id);
            ++failures;
        }
    }

    /* A raw hex: key (68 characters) fits psk= in [server] and in a
     * [client ID] block */
    {
        const char *hex = "hex:000102030405060708090a0b0c0d0e0f"
                          "101112131415161718191a1b1c1d1e1f";
        char path[] = "/tmp/test_proto_XXXXXX";
        int fd = mkstemp(path);
        FILE *fp = fd >= 0 ? fdopen(fd, "w") : NULL;
        camex_server_profile_t *c;

        if (fp == NULL) {
            fprintf(stderr, "FAIL: cannot create a temporary config\n");
            ++failures;
        } else {
            fprintf(fp, "[server]\npsk=%s\n[client CAM1]\npsk=%s\n",
                    hex, hex);
            fclose(fp);
            c = NULL;
            if (config_db_load_file(path) != 0 ||
                strcmp(server_db.globals.psk, hex) != 0 ||
                (c = config_db_find_client("CAM1")) == NULL ||
                strcmp(c->profile.psk, hex) != 0) {
                fprintf(stderr, "FAIL: a 64-digit hex: key in the config "
                        "was not read whole\n");
                ++failures;
            }
            unlink(path);
        }
    }

    /* auto_pool= values (#75) */
    {
        static const struct {
            const char *text;
            int ok;
            uint32_t net;
            uint8_t prefix;
        } pools[] = {
            { "10.200.0.0/24", 1, 0x0AC80000U, 24U },
            { "10.50.0.0/16", 1, 0x0A320000U, 16U },
            { "10.70.0.0/30", 1, 0x0A460000U, 30U },
            { "10.0.0.0/8", 1, 0x0A000000U, 8U },
            { "10.50.0.1/16", 0, 0U, 0U },     /* host bits */
            { "10.0.0.0/31", 0, 0U, 0U },
            { "10.0.0.0/7", 0, 0U, 0U },
            { "10.0.0.0", 0, 0U, 0U },
            { "10.0.0.0/", 0, 0U, 0U },
            { "10.0.0.0/16x", 0, 0U, 0U },
            { "300.0.0.0/8", 0, 0U, 0U },
            { "/16", 0, 0U, 0U },
        };
        size_t i;

        for (i = 0U; i < sizeof(pools) / sizeof(pools[0]); ++i) {
            uint32_t net = 0U;
            uint8_t prefix = 0U;
            int rc = config_parse_pool(pools[i].text, &net, &prefix);

            if ((rc == 0) != pools[i].ok ||
                (pools[i].ok && (net != pools[i].net ||
                                 prefix != pools[i].prefix))) {
                fprintf(stderr, "FAIL: auto_pool=%s: rc=%d net=%08x/%u\n",
                        pools[i].text, rc, (unsigned)net, (unsigned)prefix);
                ++failures;
            }
        }
    }

    /* A large config (#75): auto_pool=, max_clients=, 3000 [client ID]
     * blocks found by ID and by address, a repeated section merged */
    {
        char path[] = "/tmp/test_proto_XXXXXX";
        int fd = mkstemp(path);
        FILE *fp = fd >= 0 ? fdopen(fd, "w") : NULL;
        camex_server_profile_t *c;
        int i;

        if (fp == NULL) {
            fprintf(stderr, "FAIL: cannot create a temporary config\n");
            ++failures;
        } else {
            fprintf(fp, "[server]\nauto_pool=10.50.0.0/16\n"
                    "max_clients=1000\n");
            for (i = 0; i < 3000; ++i) {
                fprintf(fp, "[client CAM%04d]\nlocal_cidr=10.50.%d.%d/16\n"
                        "gateway_ip=10.50.0.1\n", i, 100 + i / 250,
                        2 + i % 250);
            }
            fprintf(fp, "[client CAM0001]\nmtu=1300\n");
            fclose(fp);
            config_db_free(&server_db);
            if (config_db_load_file(path) != 0) {
                fprintf(stderr, "FAIL: the large config was not loaded\n");
                ++failures;
            } else {
                struct in_addr a;

                if (server_db.globals.pool_net != 0x0A320000U ||
                    server_db.globals.pool_prefix != 16U ||
                    !server_db.globals.pool_set ||
                    server_db.globals.max_clients != 1000U ||
                    server_db.client_count != 3000U) {
                    fprintf(stderr, "FAIL: auto_pool=/max_clients= or the "
                            "client count were not read (%u clients)\n",
                            (unsigned)server_db.client_count);
                    ++failures;
                }
                c = config_db_find_client("cam2999");
                if (c == NULL || strcmp(c->client_id, "CAM2999") != 0 ||
                    strcmp(c->profile.local_cidr, "10.50.111.251/16") != 0) {
                    fprintf(stderr, "FAIL: CAM2999 not found by ID\n");
                    ++failures;
                }
                c = config_db_find_client("CAM0001");
                if (c == NULL || c->profile.mtu != 1300 ||
                    strcmp(c->profile.local_cidr, "10.50.100.3/16") != 0) {
                    fprintf(stderr, "FAIL: a repeated [client CAM0001] "
                            "section was not merged\n");
                    ++failures;
                }
                inet_pton(AF_INET, "10.50.106.2", &a);   /* CAM1500 */
                c = config_db_find_profile_by_ip(a.s_addr);
                if (c == NULL || strcmp(c->client_id, "CAM1500") != 0) {
                    fprintf(stderr, "FAIL: CAM1500 not found by address\n");
                    ++failures;
                }
                inet_pton(AF_INET, "10.50.99.9", &a);
                if (config_db_find_profile_by_ip(a.s_addr) != NULL ||
                    config_db_find_client("CAM3000") != NULL) {
                    fprintf(stderr, "FAIL: a missing profile was found\n");
                    ++failures;
                }
            }
            unlink(path);
        }
    }

    /* Without the keys: the pool 10.200.0.0/24, max_clients not set */
    {
        char path[] = "/tmp/test_proto_XXXXXX";
        int fd = mkstemp(path);
        FILE *fp = fd >= 0 ? fdopen(fd, "w") : NULL;

        if (fp != NULL) {
            fprintf(fp, "[server]\nport=5800\n");
            fclose(fp);
            config_db_free(&server_db);
            if (config_db_load_file(path) != 0 ||
                server_db.globals.pool_net != CAMEX_AUTO_POOL_DEFAULT_NET ||
                server_db.globals.pool_prefix != 24U ||
                server_db.globals.pool_set ||
                server_db.globals.max_clients != 0U) {
                fprintf(stderr, "FAIL: the default pool or max_clients "
                        "changed\n");
                ++failures;
            }
            unlink(path);
        }
        config_db_free(&server_db);
    }

    /* The server public key must not be all zero */
    if (crypto_set_server_pub("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=") == 0) {
        fprintf(stderr, "FAIL: an all-zero server public key was accepted\n");
        ++failures;
    }

    if (failures != 0) {
        fprintf(stderr, "test_proto: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_proto: OK\n");
    return 0;
}
