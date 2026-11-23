/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * webfonts.h — the start page's fonts (brand's brass 1.5.0: Brass Serif and
 * Brass Mono, src/fonts/OFL.txt), served by web.c at /fonts/<name>; the
 * arrays come from the .woff2 files of src/fonts at build time (webfonts.sh)
 *
 */

#ifndef CAMEX_WEBFONTS_H
#define CAMEX_WEBFONTS_H

#include <stddef.h>

typedef struct {
    const char *name;            /* "BrassMono-Regular.woff2" */
    const unsigned char *data;
    size_t len;
} web_font_t;

extern const web_font_t web_fonts[];
extern const size_t web_fonts_count;

#endif /* CAMEX_WEBFONTS_H */
