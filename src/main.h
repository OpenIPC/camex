/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * main.h — main entry, signal handlers, argument parsing
 *
 */

#ifndef CAMEX_MAIN_H
#define CAMEX_MAIN_H

#include "camex.h"

/* Parse CLI arguments */
int parse_arguments(int argc, char **argv, camex_config_t *config);

/* Validate config */
int validate_config(camex_config_t *config);

/* Validate and prepare client-specific config */
int validate_and_prepare_client(camex_config_t *config);

/* Usage / version */
void print_usage(const char *progname);
void print_version(void);

/* Signal handlers */
void signal_handler(int sig);
void sighup_handler(int sig);

#endif /* CAMEX_MAIN_H */
