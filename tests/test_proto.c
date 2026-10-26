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
#include <time.h>
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

    /* 60000 [client ID] sections with one shared psk= (#75): the key is
     * indexed once (one long probe chain made the build quadratic) and
     * names its first owner */
    {
        const char *hex = "hex:202122232425262728292a2b2c2d2e2f"
                          "303132333435363738393a3b3c3d3e3f";
        char path[] = "/tmp/test_proto_XXXXXX";
        int fd = mkstemp(path);
        FILE *fp = fd >= 0 ? fdopen(fd, "w") : NULL;

        if (fp == NULL || crypto_init(NULL) != 0) {
            fprintf(stderr, "FAIL: cannot set up the shared-key test\n");
            ++failures;
            if (fp != NULL) {
                fclose(fp);
            }
        } else {
            clock_t t0;
            double secs;
            int i;
            uint8_t key[32];

            for (i = 0; i < 60000; ++i) {
                fprintf(fp, "[client SH%05d]\npsk=%s\n", i, hex);
            }
            fclose(fp);
            config_db_free(&server_db);
            config_keystore_free(&server_ks);
            t0 = clock();
            if (config_db_load_file(path) != 0 ||
                config_build_keystore() != 0) {
                fprintf(stderr, "FAIL: the shared-key config was not "
                        "loaded\n");
                ++failures;
            } else {
                secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
                printf("shared key: 60000 entries in %.2f s\n", secs);
                fflush(stdout);
                memcpy(key, server_keystore[0].psk_key, 32);
                if (server_keystore_count != 60000U ||
                    strcmp(config_ks_owner(key), "SH00000") != 0 ||
                    !config_ks_key_owned_by(key, "SH59999") ||
                    config_ks_is_global(key) ||
                    server_ks.by_key.used != 1U) {
                    fprintf(stderr, "FAIL: shared key: %u entries, owner "
                            "%s, %u indexed by key\n",
                            (unsigned)server_keystore_count,
                            config_ks_owner(key),
                            (unsigned)server_ks.by_key.used);
                    ++failures;
                }
            }
            config_keystore_free(&server_ks);
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

    /* name= (#105): kept for its [client ID]; not valid text, too long or
     * in [defaults] stops the load */
    {
        static const char *const bad[] = {
            "[client cam1]\nname=a\x01" "b\n",
            "[client cam1]\nname=\xc3(\n",
            "[client cam1]\nname=0123456789012345678901234567890123456789"
            "0123456789012345678901234\n",
            "[defaults]\nname=all\n",
            "[server]\nname=all\n",
        };
        char path[] = "/tmp/test_proto_XXXXXX";
        int fd = mkstemp(path);
        FILE *fp = fd >= 0 ? fdopen(fd, "w") : NULL;
        size_t k;

        if (fp != NULL) {
            const camex_server_profile_t *c1, *c2;

            fprintf(fp, "[client cam1]\nname=  \xd0\xa1\xd0\xba\xd0\xbb\xd0"
                    "\xb0\xd0\xb4, gate <1>  \n[client cam2]\nmtu=1400\n");
            fclose(fp);
            config_db_free(&server_db);
            c1 = config_db_load_file(path) == 0
                     ? config_db_find_client("cam1") : NULL;
            c2 = config_db_find_client("cam2");
            if (c1 == NULL || c2 == NULL ||
                strcmp(c1->name, "\xd0\xa1\xd0\xba\xd0\xbb\xd0\xb0\xd0\xb4, "
                       "gate <1>") != 0 || c2->name[0] != '\0') {
                fprintf(stderr, "FAIL: name= not kept for its client\n");
                ++failures;
            }
            config_db_free(&server_db);
            for (k = 0U; k < sizeof(bad) / sizeof(bad[0]); ++k) {
                fp = fopen(path, "w");
                if (fp == NULL) {
                    break;
                }
                fputs(bad[k], fp);
                fclose(fp);
                if (config_db_load_file(path) == 0) {
                    fprintf(stderr, "FAIL: bad name= %u accepted\n",
                            (unsigned)k);
                    ++failures;
                }
                config_db_free(&server_db);
            }
            unlink(path);
        }
    }

    /* peer_port= (#119): the client's own, else [defaults], else 8080;
     * none = no link, either way round; not in [server] */
    {
        static const char *const bad[] = {
            "[defaults]\npeer_port=0\n",
            "[defaults]\npeer_port=-\n",
            "[client cam1]\npeer_port=65536\n",
            "[client cam1]\npeer_port=+80\n",
            "[client cam1]\npeer_port=80x\n",
            "[client cam1]\npeer_port=None\n",
            "[server]\npeer_port=8080\n",
        };
        static const struct {
            const char *conf;
            int cam1, cam2, other;
        } cases[] = {
            { "[client cam1]\nmtu=1400\n", 8080, 8080, 8080 },
            { "[defaults]\npeer_port=81\n[client cam1]\npeer_port=8443\n"
              "[client cam2]\nmtu=1400\n", 8443, 81, 81 },
            { "[defaults]\npeer_port=none\n[client cam1]\npeer_port=80\n"
              "[client cam2]\nmtu=1400\n", 80, -1, -1 },
            { "[defaults]\npeer_port=9000\n[client cam1]\npeer_port=none\n"
              "[client cam2]\nmtu=1400\n", -1, 9000, 9000 },
        };
        char path[] = "/tmp/test_proto_XXXXXX";
        int fd = mkstemp(path);
        FILE *fp = fd >= 0 ? fdopen(fd, "w") : NULL;
        size_t k;

        if (fp != NULL) {
            fclose(fp);
            for (k = 0U; k < sizeof(cases) / sizeof(cases[0]); ++k) {
                fp = fopen(path, "w");
                if (fp == NULL) {
                    break;
                }
                fputs(cases[k].conf, fp);
                fclose(fp);
                config_db_free(&server_db);
                if (config_db_load_file(path) != 0 ||
                    config_db_peer_port("cam1") != cases[k].cam1 ||
                    config_db_peer_port("CAM2") != cases[k].cam2 ||
                    config_db_peer_port("auto7") != cases[k].other) {
                    fprintf(stderr, "FAIL: peer_port= case %u: %d %d %d\n",
                            (unsigned)k, config_db_peer_port("cam1"),
                            config_db_peer_port("CAM2"),
                            config_db_peer_port("auto7"));
                    ++failures;
                }
            }
            config_db_free(&server_db);
            for (k = 0U; k < sizeof(bad) / sizeof(bad[0]); ++k) {
                fp = fopen(path, "w");
                if (fp == NULL) {
                    break;
                }
                fputs(bad[k], fp);
                fclose(fp);
                if (config_db_load_file(path) == 0) {
                    fprintf(stderr, "FAIL: bad peer_port= %u accepted\n",
                            (unsigned)k);
                    ++failures;
                }
                config_db_free(&server_db);
            }
            unlink(path);
        }
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
