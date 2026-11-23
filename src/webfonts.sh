#!/bin/sh
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# webfonts.sh <font.woff2>... — the start page's fonts (brand's brass 1.5.0,
# fonts/OFL.txt) as a C file of arrays, made at build time and never
# committed (src/Makefile): web.c serves them at /fonts/<file>
#
set -eu
echo '/* Made by webfonts.sh from the fonts in src/fonts at build time: do not edit */'
echo '#include "webfonts.h"'
n=0
for f in "$@"; do
    printf 'static const unsigned char font%d[] = {\n' "$n"
    od -An -v -tu1 "$f" | sed 's/[[:space:]][[:space:]]*/,/g; s/^,//; s/$/,/'
    echo '};'
    n=$((n + 1))
done
echo 'const web_font_t web_fonts[] = {'
n=0
for f in "$@"; do
    printf '    { "%s", font%d, sizeof(font%d) },\n' "$(basename "$f")" "$n" "$n"
    n=$((n + 1))
done
echo '};'
echo "const size_t web_fonts_count = $n;"
