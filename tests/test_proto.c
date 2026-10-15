/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * test_proto.c — unit tests for control message parsing
 *
 */

#include "camex.h"
#include "client.h"
#include "proto.h"

#include <stdio.h>
#include <string.h>

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
    if (parse("CX2P CONFIG VER=1 CLIENT_ID=cam1 MTU=1400 "
              "LOCAL_CIDR=10.200.0.5/24 GATEWAY_IP=10.200.0.1 "
              "ROUTE_CIDR=192.168.1.0/24", &cfg) != 0 ||
        strcmp(cfg.local_cidr, "10.200.0.5/24") != 0 ||
        strcmp(cfg.gateway_ip, "10.200.0.1") != 0 ||
        cfg.mtu != 1400 || cfg.route_count != 1) {
        fprintf(stderr, "FAIL: valid CONFIG not applied correctly\n");
        ++failures;
    }

    /* #12: shell metacharacters must never get through */
    expect("CX2P CONFIG LOCAL_CIDR=10.200.0.5/24 GATEWAY_IP=1;reboot",
           0, "gateway with ';'");
    expect("CX2P CONFIG LOCAL_CIDR=10.200.0.5/24 GATEWAY_IP=$(id)",
           0, "gateway with $()");
    expect("CX2P CONFIG LOCAL_CIDR=10.200.0.5/24 GATEWAY_IP=10.0.0.1|sh",
           0, "gateway with '|'");
    expect("CX2P CONFIG LOCAL_CIDR=10.200.0.5/24 GATEWAY_IP=gw.example",
           0, "gateway that is not an IPv4 address");
    expect("CX2P CONFIG LOCAL_CIDR=10.200.0.5/24;id GATEWAY_IP=10.200.0.1",
           0, "local CIDR with ';'");
    expect("CX2P CONFIG LOCAL_CIDR=10.200.0.5/33 GATEWAY_IP=10.200.0.1",
           0, "local CIDR prefix out of range");
    expect("CX2P CONFIG LOCAL_CIDR=10.200.0.5 GATEWAY_IP=10.200.0.1",
           0, "local CIDR without prefix");
    expect("CX2P CONFIG LOCAL_CIDR=10.200.0.5/24 GATEWAY_IP=10.200.0.1 "
           "ROUTE_CIDR=1.2.3.0/24;id", 0, "route with ';'");
    expect("CX2P CONFIG LOCAL_CIDR=10.200.0.5/24 GATEWAY_IP=10.200.0.1 MTU=99999",
           0, "MTU out of range");

    /* A rejected CONFIG leaves the client ID unchanged */
    {
        char buf[CAMEX_CONTROL_MAX];

        memset(&cfg, 0, sizeof(cfg));
        snprintf(cfg.client_id, sizeof(cfg.client_id), "cam1");
        snprintf(buf, sizeof(buf), "%s",
                 "CX2P CONFIG CLIENT_ID=other LOCAL_CIDR=10.200.0.5/24 "
                 "GATEWAY_IP=1;reboot");
        if (proto_parse_config(buf, &cfg) == 0 ||
            strcmp(cfg.client_id, "cam1") != 0) {
            fprintf(stderr, "FAIL: rejected CONFIG changed client ID to %s\n",
                    cfg.client_id);
            ++failures;
        }
    }

    if (failures != 0) {
        fprintf(stderr, "test_proto: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_proto: OK\n");
    return 0;
}
