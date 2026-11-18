#!/bin/sh
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-windows.sh — the CI's Windows build and Wine smoke, locally
#
# Builds camex.exe for x86_64 and i686 with MinGW as the jobs Windows and
# Wine smoke of .github/workflows/build.yml do, checks the PE type and runs
# --version and --help under Wine (win64 and win32 prefixes). Builds from a
# copy of the tree inside the container: the work tree gets no objects.
#
# Usage: docker/test-windows.sh [--rebuild]
#   --rebuild  build the camex-mingw image again (new Ubuntu packages)
#
# Environment:
#   OUT  a directory for the two .exe files (default: none, not kept)
#
# Exit codes: 0 = pass, 1 = image or build failure, 2 = smoke failure
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
IMAGE=camex-mingw

if [ "$1" = --rebuild ] || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "== building the $IMAGE image"
    docker build -q -t "$IMAGE" -f "$SCRIPT_DIR/Dockerfile.mingw" "$SCRIPT_DIR" || exit 1
fi

# The optional mount as positional parameters: a path with spaces stays one
set --
if [ -n "$OUT" ]; then
    mkdir -p "$OUT"
    set -- -v "$(cd "$OUT" && pwd):/out"
fi

docker run --rm --pull never -u "$(id -u):$(id -g)" -e HOME=/tmp/home \
    -v "$REPO_DIR:/src:ro" "$@" "$IMAGE" sh -c '
    set -e
    # Wine wants a prefix in a directory of its own user, not in /tmp
    mkdir -p "$HOME"
    fail() { echo "FAIL: $*"; exit "${2:-1}"; }
    for spec in "x86_64 x86_64-w64-mingw32 PE32+ win64" \
                "i686 i686-w64-mingw32 PE32 win32"; do
        set -- $spec
        arch=$1 host=$2 pe=$3 winearch=$4
        echo "== Windows / $arch"
        rm -rf /tmp/w && cp -a /src /tmp/w && cd /tmp/w
        make -s clean CC=$host-gcc STRIP=$host-strip TARGET=camex.exe >/dev/null
        make -s -j"$(nproc)" CC=$host-gcc STRIP=$host-strip TARGET=camex.exe ||
            fail "the $arch build"
        exe=/tmp/camex-$arch.exe
        mv camex.exe "$exe"
        file "$exe"
        file "$exe" | grep -q "$pe executable" || fail "$exe is not $pe"
        [ -d /out ] && cp "$exe" /out/

        echo "== Wine smoke / $arch"
        export WINEARCH=$winearch WINEPREFIX=$HOME/.wine-$arch WINEDEBUG=-all
        wineboot --init >/dev/null 2>&1 || true
        v=$(wine "$exe" --version 2>/dev/null) || fail "--version under Wine" 2
        echo "$v"
        echo "$v" | grep -q "[0-9]\.[0-9]*\.[0-9]" || fail "no version in: $v" 2
        h=$(wine "$exe" --help 2>/dev/null) || fail "--help under Wine" 2
        echo "$h" | head -5
        echo "$h" | grep -qi usage || fail "no usage in --help" 2
        echo "PASS: Windows / $arch"
    done
'
