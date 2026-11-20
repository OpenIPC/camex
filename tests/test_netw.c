/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * test_netw.c — named networks: IDs, pools, client addresses, the
 *               [network NAME] blocks and NET in REGISTER
 *
 */

#include "camex.h"
#include "config.h"
#include "netw.h"
#include "proto.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL: " __VA_ARGS__); \
            fputc('\n', stderr); \
            ++failures; \
        } \
    } while (0)

static uint32_t host(const char *ip)
{
    struct in_addr a;

    inet_pton(AF_INET, ip, &a);
    return ntohl(a.s_addr);
}

static const uint32_t no_reserved[2] = { 0x0AC80000U, 24U };

/* One listed network with this name and pool ("" = derived) */
static int one_network(const char *name, const char *pool)
{
    camex_net_conf_t c;

    memset(&c, 0, sizeof(c));
    netw_id_from_name(name, c.id);
    c.isolation = -1;
    if (pool[0] != '\0') {
        if (config_parse_cidr_key(pool, "pool", &c.pool_net,
                                  &c.pool_prefix) != 0) {
            return -1;
        }
        c.pool_set = 1U;
    }
    return netw_sync(&c, 1U, 0, CAMEX_NET_SUPERNET_DEFAULT_NET,
                     CAMEX_NET_SUPERNET_DEFAULT_PREFIX,
                     CAMEX_NET_POOL_PREFIX_DEFAULT, no_reserved, 1U);
}

static void test_ids(void)
{
    uint8_t a[CAMEX_NET_ID_LEN], b[CAMEX_NET_ID_LEN];
    char label[32];

    netw_id_from_name("my-cams", a);
    netw_id_from_name("my-cams", b);
    CHECK(memcmp(a, b, sizeof(a)) == 0, "the ID of a name changes");
    netw_id_from_name("My-cams", b);
    CHECK(memcmp(a, b, sizeof(a)) != 0, "names are case-sensitive");
    netw_default_label(a, label, sizeof(label));
    CHECK(strncmp(label, "net-", 4) == 0 && strlen(label) == 12,
          "default label %s", label);
    CHECK(netw_name_valid("my cams"), "a name with a space");
    CHECK(!netw_name_valid(""), "an empty name");
    CHECK(!netw_name_valid("a\tb"), "a name with a tab");
    {
        char longname[80];

        memset(longname, 'x', sizeof(longname) - 1U);
        longname[sizeof(longname) - 1U] = '\0';
        CHECK(!netw_name_valid(longname), "a name of 79 bytes");
        longname[CAMEX_NET_NAME_MAX] = '\0';
        CHECK(netw_name_valid(longname), "a name of 63 bytes");
    }
}

static void test_addresses(void)
{
    int n;
    unsigned int k;
    uint32_t base;
    char other[32];
    int found = 0;

    if (netw_alloc() != 0) {
        fprintf(stderr, "FAIL: netw_alloc\n");
        exit(1);
    }
    CHECK(one_network("lab", "10.77.0.0/24") == 0, "a /24 network");
    n = netw_find(netw_table[0].id);
    CHECK(n == 0, "the network is in slot 0");
    CHECK(netw_gateway(n) == host("10.77.0.1"), "gateway .1");
    CHECK(netw_first(n) == host("10.77.0.2"), "first .2");
    CHECK(netw_last(n) == host("10.77.0.254"), "last .254");

    /* The same ID always gets the same address, in any case */
    base = netw_candidate(n, "0203A104B5AE", 0U);
    CHECK(base != 0U && netw_has(n, base), "an address in the pool");
    CHECK(netw_candidate(n, "0203a104b5ae", 0U) == base,
          "the ID's case changes its address");
    CHECK(netw_candidate_rank(n, "0203A104B5AE", base) == 0, "rank 0");

    /* +1, -1, +2, -2 around it, inside the client addresses */
    for (k = 1U; k < 6U; ++k) {
        uint32_t c = ntohl(netw_candidate(n, "0203A104B5AE", k));
        uint32_t b = ntohl(base);
        uint32_t hosts = netw_last(n) - netw_first(n) + 1U;
        int64_t off = (k % 2U) == 1U ? (int64_t)((k + 1U) / 2U)
                                     : -(int64_t)((k + 1U) / 2U);
        int64_t want = ((int64_t)(b - netw_first(n)) + off) % (int64_t)hosts;

        if (want < 0) {
            want += hosts;
        }
        CHECK(c == netw_first(n) + (uint32_t)want,
              "candidate %u is %08x, not the expected one", k, c);
        CHECK(netw_candidate_rank(n, "0203A104B5AE", htonl(c)) == (int)k,
              "rank of candidate %u", k);
    }
    /* Every client address once, then none */
    {
        static uint8_t seen[256];
        uint32_t hosts = netw_last(n) - netw_first(n) + 1U;

        memset(seen, 0, sizeof(seen));
        for (k = 0U; k < hosts; ++k) {
            uint32_t c = ntohl(netw_candidate(n, "cam", k));

            CHECK(c >= netw_first(n) && c <= netw_last(n),
                  "candidate %u outside the pool", k);
            CHECK(!seen[c & 0xFFU], "candidate %u repeats", k);
            seen[c & 0xFFU] = 1U;
        }
        CHECK(netw_candidate(n, "cam", hosts) == 0U, "a candidate too many");
    }
    /* Two IDs whose hashes meet: the same first choice */
    for (k = 0U; k < 5000U && !found; ++k) {
        snprintf(other, sizeof(other), "ID%u", k);
        if (strcmp(other, "0203A104B5AE") != 0 &&
            netw_candidate(n, other, 0U) == base) {
            found = 1;
        }
    }
    CHECK(found, "no colliding ID in 5000 (a /24 should have one)");
    if (found) {
        CHECK(netw_candidate(n, other, 1U) ==
                  netw_candidate(n, "0203A104B5AE", 1U),
              "colliding IDs differ at +1");
    }
    /* Another network: another address for the same ID (as a rule) */
    CHECK(netw_of_ip(base) == n, "netw_of_ip of a client address");
    CHECK(netw_of_ip(htonl(host("10.78.0.5"))) == -1, "netw_of_ip outside");
}

static void test_sync(void)
{
    camex_net_conf_t c[3];
    uint32_t reserved[4] = { 0x0AC80000U, 24U, 0x0A000000U, 16U };
    int a, b;
    uint32_t pool_a;
    size_t i;

    if (netw_alloc() != 0) {
        fprintf(stderr, "FAIL: netw_alloc\n");
        exit(1);
    }
    memset(c, 0, sizeof(c));
    netw_id_from_name("alpha", c[0].id);
    netw_id_from_name("beta", c[1].id);
    c[0].isolation = c[1].isolation = -1;
    CHECK(netw_sync(c, 2U, 0, CAMEX_NET_SUPERNET_DEFAULT_NET, 8U, 16U,
                    reserved, 2U) == 0, "two derived networks");
    a = netw_find(c[0].id);
    b = netw_find(c[1].id);
    CHECK(a >= 0 && b >= 0 && a != b, "both have slots");
    CHECK(netw_table[a].pool_prefix == 16U && netw_table[b].pool_prefix == 16U,
          "derived pools are /16");
    CHECK(netw_table[a].pool_net != netw_table[b].pool_net,
          "two networks share a pool");
    CHECK((netw_table[a].pool_net & 0xFF000000U) == 0x0A000000U,
          "a pool outside 10/8");
    CHECK(netw_table[a].pool_net != 0x0A000000U &&
              netw_table[b].pool_net != 0x0A000000U,
          "a pool over the reserved 10.0/16");
    pool_a = netw_table[a].pool_net;

    /* Reload with a third one first: the old ones keep slots and pools */
    memmove(&c[1], &c[0], 2U * sizeof(c[0]));
    memset(&c[0], 0, sizeof(c[0]));
    netw_id_from_name("gamma", c[0].id);
    c[0].isolation = -1;
    CHECK(netw_sync(c, 3U, 0, CAMEX_NET_SUPERNET_DEFAULT_NET, 8U, 16U,
                    reserved, 2U) == 0, "three networks");
    CHECK(netw_find(c[1].id) == a && netw_table[a].pool_net == pool_a,
          "alpha moved after a reload");
    CHECK(netw_find(c[2].id) == b, "beta moved after a reload");

    /* An explicit pool over another one is refused, nothing changes */
    if (config_parse_cidr_key("10.0.5.0/24", "pool", &c[0].pool_net,
                              &c[0].pool_prefix) == 0) {
        c[0].pool_set = 1U;
    }
    CHECK(netw_sync(c, 3U, 0, CAMEX_NET_SUPERNET_DEFAULT_NET, 8U, 16U,
                    reserved, 2U) != 0, "a pool inside a reserved range");
    CHECK(netw_find(c[1].id) == a && netw_table[a].pool_net == pool_a,
          "a refused sync changed the table");

    /* Networks clients created: kept with allow_new, gone without */
    c[0].pool_set = 0U;
    {
        uint8_t id[CAMEX_NET_ID_LEN];
        int s;

        CHECK(netw_sync(c, 3U, 1, CAMEX_NET_SUPERNET_DEFAULT_NET, 8U, 16U,
                        reserved, 2U) == 0, "sync with allow_new");
        netw_id_from_name("made-by-a-client", id);
        s = netw_create(id);
        CHECK(s >= 0 && netw_find(id) == s && !netw_table[s].listed,
              "a created network");
        for (i = 0U; s >= 0 && i < CAMEX_NET_MAX; ++i) {
            if ((int)i != s && netw_table[i].active) {
                CHECK(netw_table[i].pool_net != netw_table[s].pool_net,
                      "a created network shares a pool");
            }
        }
        CHECK(netw_sync(c, 3U, 1, CAMEX_NET_SUPERNET_DEFAULT_NET, 8U, 16U,
                        reserved, 2U) == 0 && netw_find(id) == s,
              "a created network lost on a reload with allow_new");
        CHECK(netw_sync(c, 3U, 0, CAMEX_NET_SUPERNET_DEFAULT_NET, 8U, 16U,
                        reserved, 2U) == 0 && netw_find(id) < 0,
              "a created network kept without allow_new");
    }
    /* Listed twice */
    memcpy(c[1].id, c[2].id, sizeof(c[1].id));
    CHECK(netw_sync(c, 3U, 0, CAMEX_NET_SUPERNET_DEFAULT_NET, 8U, 16U,
                    reserved, 2U) != 0, "a network listed twice");
}

static void test_config(void)
{
    char path[] = "/tmp/camex-netw-XXXXXX";
    int fd = mkstemp(path);
    FILE *fp;
    uint8_t id[CAMEX_NET_ID_LEN];

    CHECK(fd >= 0, "mkstemp");
    if (fd < 0) {
        return;
    }
    fp = fdopen(fd, "w");
    fputs("[server]\nallow_new_networks = yes\nnetwork_supernet = "
          "10.64.0.0/10\nnetwork_prefix = 20\n\n"
          "[network my cams]\npool = 10.77.0.0/24\nclient_isolation = yes\n"
          "label = cams\n\n[network lab]\n", fp);
    fclose(fp);
    CHECK(config_db_load_file(path) == 0, "a file with networks");
    CHECK(server_db.net_count == 2U, "two [network] blocks, got %u",
          (unsigned)server_db.net_count);
    netw_id_from_name("my cams", id);
    CHECK(server_db.net_count > 0U &&
              memcmp(server_db.nets[0].id, id, sizeof(id)) == 0 &&
              server_db.nets[0].pool_set &&
              server_db.nets[0].pool_net == host("10.77.0.0") &&
              server_db.nets[0].isolation == 1 &&
              strcmp(server_db.nets[0].label, "cams") == 0,
          "[network my cams] read wrong");
    CHECK(server_db.globals.allow_new_networks &&
              server_db.globals.net_supernet == host("10.64.0.0") &&
              server_db.globals.net_super_prefix == 10U &&
              server_db.globals.net_pool_prefix == 20U,
          "[server] network keys read wrong");
    CHECK(config_networks_enabled(), "networks enabled");
    config_db_free(&server_db);

    fp = fopen(path, "w");
    fputs("[network lab]\n[network lab]\n", fp);
    fclose(fp);
    CHECK(config_db_load_file(path) != 0, "a network given twice");
    config_db_free(&server_db);

    fp = fopen(path, "w");
    fputs("[network lab]\ncolour = red\n", fp);
    fclose(fp);
    CHECK(config_db_load_file(path) != 0, "an unknown [network] key");
    config_db_free(&server_db);
    unlink(path);
}

static void test_register(void)
{
    camex_config_t c;
    char buf[CAMEX_CONTROL_MAX], id[CAMEX_CLIENT_ID_LEN], ip[16];
    uint8_t autoreq = 0U, has_net = 0U;
    uint8_t net[CAMEX_NET_ID_LEN], want[CAMEX_NET_ID_LEN];
    int wp = 0;

    memset(&c, 0, sizeof(c));
    c.auto_config = 1U;
    snprintf(c.client_id, sizeof(c.client_id), "cam1");
    snprintf(c.network, sizeof(c.network), "my cams");
    id[0] = ip[0] = '\0';
    CHECK(proto_build_register(&c, buf, sizeof(buf)) == 0 &&
              strstr(buf, " NET=") != NULL &&
              strstr(buf, "my cams") == NULL,
          "REGISTER with a network: %s", buf);
    CHECK(proto_parse_register(buf, id, sizeof(id), ip, sizeof(ip),
                               &autoreq, &wp, net, &has_net) == 0 &&
              has_net,
          "REGISTER with NET does not parse");
    netw_id_from_name("my cams", want);
    CHECK(memcmp(net, want, sizeof(net)) == 0, "NET carries another ID");

    snprintf(buf, sizeof(buf), "CAMEX REGISTER VER=2 MODE=AUTO "
             "CLIENT_ID=cam1 NET=abc\n");
    has_net = 0U;
    CHECK(proto_parse_register(buf, id, sizeof(id), ip, sizeof(ip),
                               &autoreq, &wp, net, &has_net) != 0,
          "a short NET accepted");

    c.network[0] = '\0';
    CHECK(proto_build_register(&c, buf, sizeof(buf)) == 0 &&
              strstr(buf, "NET=") == NULL,
          "NET without a network");
}

int main(void)
{
    test_ids();
    test_addresses();
    test_sync();
    test_config();
    test_register();
    if (failures != 0) {
        fprintf(stderr, "test_netw: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_netw: OK\n");
    return 0;
}
