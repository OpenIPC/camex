/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * main.c — main entry, argument parsing, signal handlers, routes
 *
 */

#define _GNU_SOURCE

#include "camex.h"
#include "client.h"
#include "server.h"
#include "main.h"
#include "config.h"
#include "crypto.h"
#include "log.h"
#include "util.h"
#include "net.h"
#include "tun.h"
#include "version.h"

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syslog.h>
#include <unistd.h>

void signal_handler(int sig)
{
    (void)sig;
    running = 0;
}

void sighup_handler(int sig)
{
    (void)sig;
    reload_config = 1;
}

void print_version(void)
{
    printf("camex %s (built %s %s)\n", CAMEX_VERSION, __DATE__, __TIME__);
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
        { NULL, 0, NULL, 0 }
    };
    int opt;
    int option_index = 0;

    opterr = 0;
    while ((opt = getopt_long(argc, argv, "M:b:an:f:l:g:p:s:c:t:k:evhP:d:T:",
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
            if (config->client_id[0] == '\0' &&
                derive_client_id(config->client_id,
                                 sizeof(config->client_id)) != 0) {
                log_message(LOG_ERR,
                    "Client name must be set with -n in auto mode");
                return -1;
            }
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
    printf("camex %s — minimal dependency-free UDP/TUN tunnel"
           " for embedded Linux\n\n", CAMEX_VERSION);

    printf("Features:\n");
    printf("  * Client/server architecture over UDP\n");
    printf("  * ChaCha20-Poly1305 authenticated encryption (optional)\n");
    printf("  * Auto-config: server pushes IP/route/MTU to the client\n");
    printf("  * Per-client pre-shared keys alongside a global PSK\n");
    printf("  * Replay protection (64-bit sliding window)\n");
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
    printf("  server    Listens on UDP and relays packets"
           " between clients\n\n");

    printf("Client options:\n");
    printf("  -a, --auto           "
           "Fetch tunnel parameters from the server\n");
    printf("  -n, --name <id>      Client ID used in auto mode\n");
    printf("  -l, --local-cidr     Local tunnel address in CIDR form\n");
    printf("  -g, --gateway-ip     Gateway inside the tunnel\n");
    printf("  -s, --server-host    Tunnel server host or address\n");
    printf("  -p, --port <port>    Tunnel server UDP port\n");
    printf("  -c, --route-cidr     Route to install through the tunnel"
           " (repeatable)\n");
    printf("  -T, --tun-dev <path> "
           "TUN device path (default: auto-detect)\n\n");

    printf("Server options:\n");
    printf("  -f, --config <path>  Server config file path"
           " (default: %s)\n", CAMEX_DEFAULT_CONFIG_PATH);
    printf("  -b, --bind-ip <addr> Optional bind address"
           " (default: 0.0.0.0)\n");
    printf("  -p, --port <port>    UDP port to listen on\n");
    printf("  -d, --bind-dev <iface>  Bind socket to a"
           " specific network interface\n\n");

    printf("Common options:\n");
    printf("  -t, --mtu <size>     Tunnel MTU, 576-9000 (default: 1500)\n");
    printf("  -k, --psk <key>      Passphrase used to derive key\n");
    printf("  -e, --encrypt        Enable ChaCha20-Poly1305 encryption\n");
    printf("  -P, --pid-file       Write PID to file on startup\n");
    printf("  -v, --version        Show version and exit\n");
    printf("  -h, --help           Show this help message and exit\n\n");

    printf("Examples:\n");
    printf("  # Server\n");
    printf("  %s --mode server --port 7000"
           " --config /etc/camex/camex.conf"
           " --encrypt --psk secret\n\n", progname);
    printf("  # Client (auto-config)\n");
    printf("  %s --mode client --auto --name 0203A104B5AE"
           " --server-host cloud.openipc.org"
           " --port 7000 --encrypt --psk secret\n\n", progname);
    printf("  # Client (manual)\n");
    printf("  %s --mode client"
           " --local-cidr 10.0.0.2/24 --gateway-ip 10.0.0.1"
           " --server-host cloud.openipc.org --port 7000\n", progname);
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
            "SECURITY WARNING: mlockall failed (%s)"
            " — sensitive key material may be swapped to disk",
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
    snprintf(config.config_path, sizeof(config.config_path), "%s",
             CAMEX_DEFAULT_CONFIG_PATH);

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
            if (camex_add_route(current_config.route_cidrs[i],
                                current_config.gateway_ip) != 0) {
                log_message(LOG_WARNING, "Route setup failed for %s",
                            current_config.route_cidrs[i]);
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
