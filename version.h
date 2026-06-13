/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * version.h — shared version string for userspace app and kernel module
 *
 * The build system (-DCAMEX_VERSION="...") overrides this fallback.
 * Used by both camex.c (userspace) and camex-k.c (kernel module).
 *
 */

#ifndef CAMEX_VERSION_H
#define CAMEX_VERSION_H

#ifndef CAMEX_VERSION
#define CAMEX_VERSION "2.0.0"
#endif

#endif /* CAMEX_VERSION_H */
