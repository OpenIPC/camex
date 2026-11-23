#!/bin/sh
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# webfonts.sh <font.woff2>... — the start page's fonts (brand's brass 1.5.0,
# fonts/OFL.txt) as a C file of arrays, made at build time and never
# committed (src/Makefile): web.c serves them at /fonts/<file>
#
set -eu
[ $# -gt 0 ] || { echo "webfonts.sh: no fonts" >&2; exit 1; }
for f in "$@"; do   # od's own failure would not stop the pipe below
    [ -s "$f" ] && [ -r "$f" ] || { echo "webfonts.sh: cannot read $f" >&2; exit 1; }
done
echo '/* Made by webfonts.sh from the fonts in src/fonts at build time: do not edit */'
echo '#include "webfonts.h"'
n=0
for f in "$@"; do
    printf 'static const unsigned char font%d[] = {\n' "$n"
    bytes=$(od -An -v -tu1 "$f") || { echo "webfonts.sh: cannot read $f" >&2; exit 1; }
    # awk splits on any run of blanks: BSD od pads its last line with them
    printf '%s\n' "$bytes" | awk '{ for (i = 1; i <= NF; i++) { printf "%s,", $i
        if (++n % 16 == 0) printf "\n" } } END { if (n % 16) printf "\n" }'
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
