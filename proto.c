/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * proto.c — control protocol (REGISTER / CONFIG message builders and parsers)
 *
 */

#include "proto.h"
#include "client.h"
#include "crypto.h"
#include "log.h"
#include "util.h"

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

int proto_build_register(const camex_config_t *config,
                         char *buffer, size_t size)
{
    size_t offset = 0U;

    if (config == NULL || buffer == NULL || size == 0U) {
        return -1;
    }

    if (append_control_field(buffer, size, &offset,
                             "%s REGISTER", CAMEX_MAGIC) != 0) {
        return -1;
    }

    /* Append protocol version */
    if (append_control_field(buffer, size, &offset,
                             " VER=%u", (unsigned)CAMEX_PROTO_VER) != 0) {
        return -1;
    }

    if (config->auto_config && !client_state.config_received) {
        if (append_control_field(buffer, size, &offset,
                                 " MODE=AUTO CLIENT_ID=%s",
                                 config->client_id) != 0) {
            return -1;
        }
    } else {
        if (append_control_field(buffer, size, &offset,
                                 " MODE=MANUAL LOCAL_IP=%s",
                                 config->local_ip) != 0) {
            return -1;
        }
    }

    if (append_control_field(buffer, size, &offset, "\n") != 0) {
        return -1;
    }

    return 0;
}

int proto_parse_register(char *message,
                         char *client_id, size_t client_id_size,
                         char *local_ip, size_t local_ip_size,
                         uint8_t *auto_request)
{
    char *saveptr = NULL;
    char *token;
    int saw_mode = 0;
    unsigned int proto_ver = 0;

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

        if (token_value(token, "VER", value, sizeof(value)) == 0) {
            proto_ver = (unsigned int)atoi(value);
            if (proto_ver > CAMEX_PROTO_VER) {
                log_message(LOG_WARNING,
                            "Client protocol version %u > server %u",
                            proto_ver, (unsigned)CAMEX_PROTO_VER);
            }
            continue;
        }

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
            if (copy_option(client_id, client_id_size, value,
                            "client ID") != 0) {
                return -1;
            }
            continue;
        }

        if (token_value(token, "LOCAL_IP", value, sizeof(value)) == 0) {
            if (copy_option(local_ip, local_ip_size, value,
                            "local IP") != 0) {
                return -1;
            }
            continue;
        }

        if (!saw_mode && client_id != NULL && *client_id == '\0' &&
            local_ip != NULL && *local_ip == '\0') {
            if (validate_ipv4(token, "client IP") != 0) {
                return -1;
            }
            if (copy_option(local_ip, local_ip_size, token,"local IP") != 0) {
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
            log_message(LOG_ERR,
                        "Missing client ID in auto register request");
            return -1;
        }
    } else if (local_ip == NULL || *local_ip == '\0') {
        log_message(LOG_ERR,
                    "Missing local IP in manual register request");
        return -1;
    }

    return 0;
}

int proto_build_config(const char *client_id,
                       const camex_profile_t *profile,
                       char *buffer, size_t size)
{
    size_t offset = 0U;
    size_t i;

    if (buffer == NULL || profile == NULL || size == 0U) {
        return -1;
    }

    if (append_control_field(buffer, size, &offset,
                             "%s CONFIG", CAMEX_MAGIC) != 0) {
        return -1;
    }

    /* Append protocol version */
    if (append_control_field(buffer, size, &offset,
                             " VER=%u", (unsigned)CAMEX_PROTO_VER) != 0) {
        return -1;
    }

    if (client_id != NULL && *client_id != '\0') {
        if (append_control_field(buffer, size, &offset,
                                 " CLIENT_ID=%s", client_id) != 0) {
            return -1;
        }
    }
    if (profile->mtu > 0) {
        if (append_control_field(buffer, size, &offset,
                                 " MTU=%d", profile->mtu) != 0) {
            return -1;
        }
    }
    if (profile->local_cidr[0] != '\0') {
        if (append_control_field(buffer, size, &offset,
                                 " LOCAL_CIDR=%s",
                                 profile->local_cidr) != 0) {
            return -1;
        }
    }
    if (profile->gateway_ip[0] != '\0') {
        if (append_control_field(buffer, size, &offset,
                                 " GATEWAY_IP=%s",
                                 profile->gateway_ip) != 0) {
            return -1;
        }
    }
    for (i = 0; i < profile->route_count; ++i) {
        if (append_control_field(buffer, size, &offset,
                                 " ROUTE_CIDR=%s",
                                 profile->route_cidrs[i]) != 0) {
            return -1;
        }
    }
    if (append_control_field(buffer, size, &offset, "\n") != 0) {
        return -1;
    }

    return 0;
}

int proto_parse_config(char *message, camex_config_t *config)
{
    char *saveptr = NULL;
    char *token;
    camex_profile_t profile;

    if (message == NULL || config == NULL) {
        return -1;
    }

    config_profile_reset(&profile);

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
            if (copy_option(config->client_id, sizeof(config->client_id),
                            value, "client ID") != 0) {
                return -1;
            }
            continue;
        }
        if (token_value(token, "LOCAL_CIDR", value, sizeof(value)) == 0) {
            if (copy_option(profile.local_cidr, sizeof(profile.local_cidr),
                            value, "local CIDR") != 0) {
                return -1;
            }
            continue;
        }
        if (token_value(token, "GATEWAY_IP", value, sizeof(value)) == 0) {
            if (copy_option(profile.gateway_ip, sizeof(profile.gateway_ip),
                            value, "gateway IP") != 0) {
                return -1;
            }
            continue;
        }
        if (token_value(token, "ROUTE_CIDR", value, sizeof(value)) == 0) {
            if (config_profile_append_route(&profile, value) != 0) {
                return -1;
            }
            continue;
        }
        if (token_value(token, "MTU", value, sizeof(value)) == 0) {
            if (parse_mtu(value, &profile.mtu) != 0) {
                log_message(LOG_ERR,
                            "Invalid MTU in config response: %s", value);
                return -1;
            }
            continue;
        }

        log_message(LOG_ERR, "Unknown config token: %s", token);
        return -1;
    }

    if (config_apply_profile(config, &profile) != 0) {
        log_message(LOG_ERR, "Invalid config response");
        return -1;
    }

    return 0;
}
