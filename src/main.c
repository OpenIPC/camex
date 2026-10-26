/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * main.c — main entry, argument parsing, signal handlers, routes
 *
 */

#include "camex.h"
#include "client.h"
#include "server.h"
#include "main.h"
#include "config.h"
#include "crypto.h"
#include "keys.h"
#include "log.h"
#include "util.h"
#include "net.h"
#include "packet.h"
#include "tun.h"
#include "version.h"
#include "control.h"

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#endif
#ifdef __linux__
#include <sys/prctl.h>
#endif
#ifndef _WIN32
#include <sys/mman.h>
#include <sys/syslog.h>
#endif

void signal_handler(int sig)
{
    (void)sig;
    running = 0;
}

#ifndef _WIN32
void sighup_handler(int sig)
{
    (void)sig;
    reload_config = 1;
}
#endif

void print_version(void)
{
    printf("camex %s\n", CAMEX_VERSION);
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

int parse_arguments(int argc, char **argv, camex_config_t *config)
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
        { "transport", required_argument, NULL, 'R' },
        { "keepalive", required_argument, NULL, 'K' },
        { "server-timeout", required_argument, NULL, 'W' },
        { "genkey", no_argument, NULL, 'G' },
        { "server-key-file", required_argument, NULL, 'S' },
        { "server-pubkey", required_argument, NULL, 'Y' },
        { "status", no_argument, NULL, 'Q' },
        { "json", no_argument, NULL, 'J' },
        { "control", required_argument, NULL, 'C' },
        { "client-isolation", no_argument, NULL, 'I' },
        { "format", required_argument, NULL, 'F' },
        { "domain", required_argument, NULL, 'D' },
        { "upstream-port", required_argument, NULL, 'U' },
        { "default", required_argument, NULL, 'E' },
        { NULL, 0, NULL, 0 }
    };
    int opt;
    int option_index = 0;

    opterr = 0;
    while ((opt = getopt_long(argc, argv, "M:b:an:f:l:g:p:s:c:t:k:evhP:d:T:R:K:W:GS:Y:QJC:IF:D:U:E:",
                             long_options, &option_index)) != -1) {
        switch (opt) {
        case 'M':
            if (parse_mode(optarg, &config->mode) != 0) {
                log_message(LOG_ERR, "Invalid mode: %s", optarg);
                return -1;
            }
            break;
        case 'b':
            if (copy_option(config->bind_ip, sizeof(config->bind_ip),
                            optarg, "bind IP") != 0) {
                return -1;
            }
            break;
        case 'a':
            config->auto_config = 1U;
            break;
        case 'n':
            if (copy_option(config->client_id, sizeof(config->client_id),
                            optarg, "client ID") != 0) {
                return -1;
            }
            break;
        case 'f':
            if (copy_option(config->config_path, sizeof(config->config_path),
                            optarg, "config path") != 0) {
                return -1;
            }
            break;
        case 'l':
            if (copy_option(config->local_cidr, sizeof(config->local_cidr),
                            optarg, "local CIDR") != 0) {
                return -1;
            }
            break;
        case 'g':
            if (copy_option(config->gateway_ip, sizeof(config->gateway_ip),
                            optarg, "gateway IP") != 0) {
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
            if (copy_option(config->server_host, sizeof(config->server_host),
                            optarg, "server host") != 0) {
                return -1;
            }
            break;
        case 'c':
            if (append_route_option(config->route_cidrs, &config->route_count,
                                    optarg, "route CIDR") != 0) {
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
            if (copy_option(config->psk, sizeof(config->psk),
                            optarg, "PSK") != 0) {
                return -1;
            }
            break;
        case 'e':
            config->encrypt = 1U;
            break;
        case 'v':
            print_version();
            return 1;
        case 'G':
            {
                char priv[KEY_B64_LEN + 1U];
                char pub[KEY_B64_LEN + 1U];

                if (key_generate(priv, pub) != 0) {
                    log_message(LOG_ERR, "Cannot generate a key pair");
                    return -1;
                }
                printf("server_key=%s\npublic_key=%s\n", priv, pub);
                crypto_wipe(priv, sizeof(priv));
            }
            return 1;
        case 'S':
            if (copy_option(config->server_key_file,
                            sizeof(config->server_key_file), optarg,
                            "server key file") != 0) {
                return -1;
            }
            break;
        case 'Y':
            if (copy_option(config->server_pubkey,
                            sizeof(config->server_pubkey), optarg,
                            "server public key") != 0) {
                return -1;
            }
            break;
        case 'h':
            print_usage(argv[0]);
            return 1;
        case 'P':
            if (copy_option(config->pid_file, sizeof(config->pid_file),
                            optarg, "PID file") != 0) {
                return -1;
            }
            break;
        case 'd':
            if (copy_option(config->bind_dev, sizeof(config->bind_dev),
                            optarg, "bind device") != 0) {
                return -1;
            }
            break;
        case 'T':
            if (copy_option(config->tun_dev, sizeof(config->tun_dev),
                            optarg, "TUN device") != 0) {
                return -1;
            }
            break;
        case 'R':
            if (strcmp(optarg, "tcp") == 0) {
                config->transport = CAMEX_TRANSPORT_TCP;
            } else if (strcmp(optarg, "udp") == 0) {
                config->transport = CAMEX_TRANSPORT_UDP;
            } else {
                log_message(LOG_ERR, "Invalid transport: %s (use tcp or udp)",
                            optarg);
                return -1;
            }
            break;
        case 'K':
            {
                char *end = NULL;
                long val = strtol(optarg, &end, 10);
                if (end == optarg || *end != '\0' ||
                    ((val == LONG_MAX || val == LONG_MIN) && errno == ERANGE)) {
                    log_message(LOG_ERR,
                                "Keepalive must be an integer 0..3600, got: %s",
                                optarg);
                    return -1;
                }
                if (val < 0 || val > 3600) {
                    log_message(LOG_ERR,
                                "Keepalive must be 0..3600 seconds, got: %s",
                                optarg);
                    return -1;
                }
                config->keepalive = (int)val;
            }
            break;
        case 'W':
            {
                char *end = NULL;
                long val = strtol(optarg, &end, 10);
                if (end == optarg || *end != '\0' ||
                    ((val == LONG_MAX || val == LONG_MIN) && errno == ERANGE)) {
                    log_message(LOG_ERR,
                                "Server timeout must be an integer 5..3600, got: %s",
                                optarg);
                    return -1;
                }
                if (val < 5 || val > 3600) {
                    log_message(LOG_ERR,
                                "Server timeout must be 5..3600 seconds, got: %s",
                                optarg);
                    return -1;
                }
                config->server_timeout = (int)val;
            }
            break;
        case 'Q':
            config->status_query = 1U;
            break;
        case 'I':
            config->client_isolation = 1U;
            break;
        case 'F':
            /* The last --format (or --json) wins */
            config->status_nginx_map = 0U;
            config->status_nginx_index = 0U;
            config->status_json = 0U;
            if (strcmp(optarg, "nginx-map") == 0) {
                config->status_nginx_map = 1U;
            } else if (strcmp(optarg, "nginx-index") == 0) {
                config->status_nginx_index = 1U;
            } else if (strcmp(optarg, "json") == 0) {
                config->status_json = 1U;
            } else if (strcmp(optarg, "table") != 0) {
                log_message(LOG_ERR, "Invalid --format: %s (table, json, "
                            "nginx-map or nginx-index)", optarg);
                return -1;
            }
            break;
        case 'D':
            if (copy_option(config->map_domain, sizeof(config->map_domain),
                            optarg, "domain") != 0) {
                return -1;
            }
            break;
        case 'U':
            {
                char *end = NULL;
                long val = strtol(optarg, &end, 10);

                if (end == optarg || *end != '\0' || val < 1 || val > 65535) {
                    log_message(LOG_ERR, "--upstream-port must be 1..65535, "
                                "got: %s", optarg);
                    return -1;
                }
                config->map_port = (int)val;
            }
            break;
        case 'E':
            if (copy_option(config->map_default, sizeof(config->map_default),
                            optarg, "default") != 0) {
                return -1;
            }
            break;
        case 'J':
            config->status_json = 1U;
            config->status_nginx_map = 0U;
            config->status_nginx_index = 0U;
            break;
        case 'C':
            if (copy_option(config->control_path,
                            sizeof(config->control_path), optarg,
                            "control socket") != 0) {
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

int validate_config(camex_config_t *config)
{
    if (config == NULL) {
        return -1;
    }

    if (config->mtu < 576 || config->mtu > 9000) {
        log_message(LOG_ERR, "MTU must be in the range 576..9000");
        return -1;
    }

    if (config->transport != CAMEX_TRANSPORT_UDP &&
        config->transport != CAMEX_TRANSPORT_TCP) {
        log_message(LOG_ERR, "Transport must be 0 (udp) or 1 (tcp)");
        return -1;
    }

    /* Protocol v2 is always encrypted: a client needs its key. A server
     * may do without a global key when every client has its own psk=. */
    if (config->mode == CAMEX_MODE_CLIENT && config->psk[0] == '\0') {
        log_message(LOG_ERR, "The client needs a key: --psk");
        return -1;
    }

    /* Each side has its own key option: a misplaced one is a mistake */
    if (config->mode == CAMEX_MODE_CLIENT &&
        config->server_key_file[0] != '\0') {
        log_message(LOG_ERR, "--server-key-file is for the server; the "
                    "client needs --server-pubkey");
        return -1;
    }
    if (config->mode == CAMEX_MODE_SERVER && config->server_pubkey[0] != '\0') {
        log_message(LOG_ERR, "--server-pubkey is for the client; the server "
                    "needs --server-key-file");
        return -1;
    }

    switch (config->mode) {
    case CAMEX_MODE_CLIENT:
        if (config->server_host[0] == '\0' || config->port <= 0) {
            log_message(LOG_ERR, "Server host and port must be specified");
            return -1;
        }

        if (config->auto_config) {
            /* Without --name the ID is found in camex_init(), after the
             * keys were checked (#121) */
        } else {
            char local_ip[16];
            char local_netmask[16];

            if (config->local_cidr[0] == '\0') {
                log_message(LOG_ERR, "Local CIDR must be specified");
                return -1;
            }
            if (config->gateway_ip[0] == '\0') {
                log_message(LOG_ERR, "Gateway IP must be specified");
                return -1;
            }
            if (parse_local_cidr(config->local_cidr, local_ip,
                                 sizeof(local_ip),
                                 local_netmask,
                                 sizeof(local_netmask)) != 0) {
                return -1;
            }
            if (validate_ipv4(config->gateway_ip, "gateway IP") != 0) {
                return -1;
            }

            if (validate_route_list(config->route_cidrs,
                                    config->route_count) != 0) {
                return -1;
            }
        }

        break;
    case CAMEX_MODE_SERVER:
        if (config->auto_config) {
            log_message(LOG_ERR,
                        "Auto mode is only available for client mode");
            return -1;
        }
        break;
    default:
        log_message(LOG_ERR, "Unsupported mode");
        return -1;
    }

    return 0;
}

int validate_and_prepare_client(camex_config_t *config)
{
    char local_ip[16];
    char local_netmask[16];

    if (parse_local_cidr(config->local_cidr, local_ip, sizeof(local_ip),
                         local_netmask, sizeof(local_netmask)) != 0) {
        return -1;
    }

    snprintf(config->local_ip, sizeof(config->local_ip), "%s", local_ip);
    if (validate_ipv4(config->gateway_ip, "gateway IP") != 0) {
        return -1;
    }

    if (validate_route_list(config->route_cidrs, config->route_count) != 0) {
        return -1;
    }

    return 0;
}

void print_usage(const char *progname)
{
    printf("camex %s — minimal dependency-free UDP/TCP tunnel"
           " for embedded Linux\n\n", CAMEX_VERSION);

    printf("Features:\n");
    printf("  * Client/server architecture over UDP or TCP\n");
    printf("  * Protocol v2: Noise_NKpsk0_25519_ChaChaPoly_BLAKE2b handshake,\n"
           "    fresh keys per session and direction, always encrypted\n");
    printf("  * Auto-config: server pushes IP/route/MTU to the client\n");
    printf("  * Per-client pre-shared keys alongside a global PSK\n");
    printf("  * Replay protection (2048-packet window), roaming, rekeying\n");
    printf("  * Works with standard tun.ko (/dev/net/tun)"
           " or camex.ko (/dev/camex)\n");
    printf("  * Cross-compilable, no external dependencies\n");
    printf("  * Packet memory locked (mlockall), core dumps disabled\n\n");

    printf("Usage:\n");
    printf("  %s --mode client --server-host <addr>"
           " --port <port> [options]\n", progname);
    printf("  %s --mode client --auto --server-host <addr>"
           " --port <port> [options]\n", progname);
    printf("  %s --mode server --port <port> [options]\n\n", progname);

    printf("Mode selection:\n");
    printf("  -M, --mode <mode>    "
           "Operation mode: client or server (required)\n\n");

    printf("Modes:\n");
    printf("  client    Creates a TUN device and connects outbound"
           " to the server\n");
    printf("  server    Listens on UDP/TCP and relays packets"
           " between clients\n\n");

    printf("Client options:\n");
    printf("  -a, --auto              "
           "Fetch tunnel parameters from the server\n");
    printf("  -n, --name <id>         Client ID (auto mode; optional in manual mode)\n");
    printf("  -l, --local-cidr <cidr>  Local tunnel address in CIDR form\n");
    printf("  -g, --gateway-ip <addr>  Gateway inside the tunnel\n");
    printf("  -s, --server-host <addr> Tunnel server host or address\n");
    printf("  -p, --port <port>       Tunnel server port\n");
    printf("  -c, --route-cidr <cidr>  Route to install through the"
           " tunnel (repeatable)\n");
    printf("  -T, --tun-dev <path>   TUN device path"
           " (default: auto-detect)\n\n");

    printf("Server options:\n");
    printf("  -f, --config <path>  Server config file path"
           " (default: %s)\n", CAMEX_DEFAULT_CONFIG_PATH);
    printf("  -b, --bind-ip <addr> Optional bind address"
           " (default: 0.0.0.0)\n");
    printf("  -p, --port <port>    Port to listen on\n");
    printf("  -d, --bind-dev <iface>  Bind socket to a"
           " specific network interface\n");
    printf("  -T, --tun-dev <path> TUN device path"
           " (default: auto-detect)\n");
    printf("      --client-isolation  Drop client-to-client packets"
           " (or client_isolation=yes in [server])\n");
    printf("      --control <path> Control socket for --status"
           " (default: %s/camex-<udp|tcp>-<port>.sock;"
           " none = off)\n\n", CAMEX_CONTROL_DIR);

    printf("Status of a running server (on its host):\n");
    printf("      --status         List the connected clients of every"
           " server on this host\n");
    printf("      --json           The same as JSON, for scripts\n");
    printf("      --control <path> Ask only the server at this socket\n");
    printf("      --format nginx-map  An nginx map of the clients"
           " (<id>[.<domain>] -> <address>:<port>)\n");
    printf("      --format nginx-index  A start page of the clients (HTML:"
           " Configured and Auto tabs; name,\n"
           "                       link, address, peer link (peer_port=),"
           " connection, traffic)\n");
    printf("      --domain <name>  With nginx-map and nginx-index: the"
           " domain after the client ID\n"
           "                       (default: domain= in [server] of the"
           " server)\n");
    printf("      --upstream-port <port>  With nginx-map: the clients'"
           " web port (default: 80)\n");
    printf("      --default <ip:port>  With nginx-map: where unknown"
           " names go\n\n");

    printf("Common options:\n");
    printf("  -t, --mtu <size>     Tunnel MTU, 576-9000 (default: 1500)\n");
    printf("  -k, --psk <key>      Pre-shared key: hex:<64 hex digits>,"
           " base64:<32 bytes>,\n"
           "                       or a passphrase (stretched with Argon2id)\n");
    printf("  -e, --encrypt        Accepted for compatibility: protocol v2 is"
           " always encrypted\n");
    printf("  -P, --pid-file <path> Write PID to file on startup\n");
    printf("  -R, --transport <t>  Transport protocol: udp (default) or tcp\n");
    printf("  -K, --keepalive <s>  Keepalive interval in seconds (default:"
           " 10); 0: one every 60 s,\n"
           "                       a lost link noticed after 60 s +"
           " --server-timeout\n");
    printf("      --genkey         Print a new server key pair and exit\n");
    printf("      --server-key-file <path> Server: file with its private key\n"
           "                       (or server_key= in [server])\n");
    printf("      --server-pubkey <key> Client: the server's public key\n");
    printf("  -W, --server-timeout <s> Connection timeout in seconds"
           " (default: 20)\n");
    printf("  -v, --version        Show version and exit\n");
    printf("  -h, --help           Show this help message and exit\n\n");

    printf("Examples:\n");
    printf("  # Server key pair (once): keep server_key secret, give\n"
           "  # public_key to the clients\n");
    printf("  (umask 077; %s --genkey > /etc/camex/server.key)\n\n", progname);
    printf("  # Server (UDP)\n");
    printf("  %s --mode server --port 5800"
           " --config /etc/camex/camex.conf"
           " --server-key-file /etc/camex/server.key --psk secret\n\n",
           progname);
    printf("  # Server (TCP)\n");
    printf("  %s --mode server --port 5800 --transport tcp"
           " --config /etc/camex/camex.conf"
           " --server-key-file /etc/camex/server.key --psk secret\n\n",
           progname);
    printf("  # Client (auto-config, UDP)\n");
    printf("  %s --mode client --auto --name 0203A104B5AE"
           " --server-host cloud.openipc.org --port 5800"
           " --server-pubkey <public_key> --psk secret\n\n", progname);
    printf("  # Client (manual)\n");
    printf("  %s --mode client"
           " --local-cidr 10.0.0.2/24 --gateway-ip 10.0.0.1"
           " --server-host cloud.openipc.org --port 5800"
           " --server-pubkey <public_key> --psk secret\n", progname);
}

int main(int argc, char *argv[])
{
    camex_config_t config;
    int parse_result;

#ifdef _WIN32
    {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            fprintf(stderr, "WSAStartup failed\n");
            return 1;
        }
    }
#endif

#ifndef _WIN32
    openlog("camex", LOG_PID, LOG_DAEMON);
#endif
#ifdef __linux__
    prctl(PR_SET_DUMPABLE, 0);
#endif
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, sighup_handler);
#endif

    memset(&config, 0, sizeof(config));
    config.mode = CAMEX_MODE_CLIENT;
    config.mtu = 1500;
    config.server_timeout = 20;
    config.keepalive = 10;
    snprintf(config.config_path, sizeof(config.config_path), "%s",
             CAMEX_DEFAULT_CONFIG_PATH);

    if (argc == 1) {
        print_usage(argv[0]);
#ifndef _WIN32
        closelog();
#endif
        return 0;
    }

    parse_result = parse_arguments(argc, argv, &config);
    if (parse_result == 0 && config.status_query) {
        /* camex --status (#82): ask the running server(s), nothing else */
        int rc = config.status_nginx_map
                     ? control_nginx_map(config.control_path,
                                         config.map_domain,
                                         config.map_port > 0 ? config.map_port
                                                             : 80,
                                         config.map_default)
                 : config.status_nginx_index
                     ? control_nginx_index(config.control_path,
                                           config.map_domain)
                     : control_status(config.control_path,
                                      config.status_json);

#ifndef _WIN32
        closelog();
#endif
        return rc;
    }
    if (parse_result != 0) {
        if (parse_result > 0) {
#ifndef _WIN32
            closelog();
#endif
            return 0;
        }

        print_usage(argv[0]);
#ifndef _WIN32
        closelog();
#endif
        return 1;
    }

    if (validate_config(&config) != 0) {
        print_usage(argv[0]);
#ifndef _WIN32
        closelog();
#endif
        return 1;
    }

    /* Lock pages now that we are in real operation mode
     * (skip for --help/--version which never reach here). */
#ifndef _WIN32
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        fprintf(stderr,
            "SECURITY WARNING: mlockall failed (%s)"
            " — sensitive key material may be swapped to disk\n",
            strerror(errno));
    } else {
        key_set_memory_locked(1);
    }
    /* Line-buffered output for predictable log ordering */
    setlinebuf(stdout);
    setlinebuf(stderr);
#endif

    if (camex_init(&config) != 0) {
        log_message(LOG_ERR, "Initialization failed");
        camex_cleanup();
#ifndef _WIN32
        closelog();
#endif
        return 1;
    }

    if (config.pid_file[0] != '\0') {
        FILE *pf = fopen(config.pid_file, "w");
        if (pf != NULL) {
#ifdef _WIN32
            fprintf(pf, "%lu\n", (unsigned long)GetCurrentProcessId());
#else
            fprintf(pf, "%d\n", (int)getpid());
#endif
            fclose(pf);
        } else {
            print_errno_message(LOG_WARNING, "Cannot write PID file");
        }
    }

    /* An externally provided TUN (Android) gets its routes from the host
     * application, which camex has no permission to change; other packet
     * backends may have no host interface at all. */
    if (client_mode) {
        camex_client_install_routes();
    }

    camex_run();
    camex_stop();
    if (config.pid_file[0] != '\0') {
        unlink(config.pid_file);
    }
#ifndef _WIN32
    closelog();
#endif
    return 0;
}

/* JNI entry point (Android) — binary-compatible with main() */
int camex_main(int argc, char *argv[])
{
    return main(argc, argv);
}
