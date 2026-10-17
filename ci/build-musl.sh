#!/bin/sh
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# build-musl.sh — static musl build of camex for one Linux target (#54)
#
#   ci/build-musl.sh <target> [outdir]
#
# target: arm      OpenIPC toolchain (hisilicon-hi3516cv100, arm musleabi) — 32-bit ARM cameras
#         mipsel   OpenWrt toolchain (ramips/mt7621; musl, soft-float): runs on every MIPS32r2
#                  core with or without an FPU — Ingenic cameras and MediaTek routers, whose
#                  kernels often lack FPU emulation, so hard-float code dies with SIGILL
#         aarch64 | x86_64   Bootlin musl toolchains
#
# Produces <outdir>/camex-<target>: static, stripped, not packed. The same toolchains as
# OpenIPC/vtesse ci/build-musl.sh. musl is used because a static glibc binary cannot
# resolve host names (getaddrinfo) without the NSS libraries of the same glibc on the
# device, and OpenIPC firmware has no glibc at all. With QEMU=1 the unit tests and
# `camex --version` are run under qemu-user as well.
set -eu

TARGET=$1
OUT=${2:-dist}
BOOTLIN_VERSION=${BOOTLIN_VERSION:-stable-2026.08-1}
OPENWRT_VERSION=${OPENWRT_VERSION:-23.05.5}
# The archives are checked before their compiler is run: SHA256 pinned below for the
# default Bootlin and OpenWrt versions (TOOLCHAIN_SHA256 overrides it, required with
# another version); the OpenIPC toolchain is a rolling release asset, checked
# against the digest GitHub reports for it.
CACHE=${TOOLCHAIN_CACHE:-$HOME/.cache/camex-toolchains}
ROOT=$(cd "$(dirname "$0")/.." && pwd)

OPENIPC=https://github.com/OpenIPC/firmware/releases/download/toolchain
BOOTLIN=https://toolchains.bootlin.com/downloads/releases/toolchains
OPENWRT=https://downloads.openwrt.org/releases/$OPENWRT_VERSION/targets
SUBDIR=     # toolchain directory inside the archive, if not its top level
case "$TARGET" in
    arm)
        URL=$OPENIPC/toolchain.hisilicon-hi3516cv100.tgz
        DIRNAME=arm-openipc-linux-musleabi_sdk-buildroot; QEMU_BIN=qemu-arm; SHA=github ;;
    mipsel)
        OW=ramips/mt7621; OWA=mipsel_24kc; QEMU_BIN=qemu-mipsel
        SHA=bb08478a46869009a2a8b85f88e266cc6b6d3ccefc34de61907ead2a986e08c9
        [ "$OPENWRT_VERSION" = 23.05.5 ] || SHA=
        DIRNAME=openwrt-toolchain-$OPENWRT_VERSION-$(echo $OW | tr / -)_gcc-12.3.0_musl.Linux-x86_64
        URL=$OPENWRT/$OW/$DIRNAME.tar.xz
        SUBDIR=/toolchain-${OWA}_gcc-12.3.0_musl ;;
    aarch64|x86_64)
        if [ "$TARGET" = x86_64 ]; then TC=x86-64; QEMU_BIN=qemu-x86_64
            SHA=78d3a4683d6ac47b5ee73bd5bce210b55eb93dff1b137c61298af97eb0d2b5a6
        else TC=aarch64; QEMU_BIN=qemu-aarch64
            SHA=b388c480a48e8e9f9b99e3d14e69219c4d61e5a2424a82faecb88a015b781a60; fi
        [ "$BOOTLIN_VERSION" = stable-2026.08-1 ] || SHA=
        DIRNAME=$TC--musl--$BOOTLIN_VERSION
        URL=$BOOTLIN/$TC/tarballs/$DIRNAME.tar.xz ;;
    *) echo "unknown target: $TARGET (arm, mipsel, aarch64, x86_64)" >&2; exit 2 ;;
esac

DIR=$CACHE/$DIRNAME$SUBDIR
if [ ! -d "$DIR/bin" ]; then
    mkdir -p "$CACHE"
    echo "downloading $URL"
    # download first (with retries), then extract: a truncated stream piped
    # into tar looks like a corrupt archive
    curl -fsSL --retry 3 -o "$CACHE/toolchain.tar" "$URL"
    SHA=${TOOLCHAIN_SHA256:-$SHA}
    if [ "$SHA" = github ]; then
        # authenticated when a token is available: anonymous calls share a 60/hour limit per IP
        command -v jq >/dev/null ||
            { echo "jq is needed to verify $URL (or set TOOLCHAIN_SHA256)" >&2; rm -f "$CACHE/toolchain.tar"; exit 1; }
        TOKEN=${GITHUB_TOKEN:-${GH_TOKEN:-}}
        REL=$(curl -fsSL --retry 3 ${TOKEN:+-H "Authorization: Bearer $TOKEN"} \
              "https://api.github.com/repos/OpenIPC/firmware/releases/tags/toolchain") ||
            { echo "GitHub API request for the toolchain digest failed (rate limit?)" >&2; rm -f "$CACHE/toolchain.tar"; exit 1; }
        SHA=$(printf '%s' "$REL" | jq -r --arg n "${URL##*/}" '.assets[] | select(.name == $n) | .digest // "" | sub("^sha256:"; "")')
    fi
    [ -n "$SHA" ] || { echo "no SHA256 for $URL: set TOOLCHAIN_SHA256" >&2; rm -f "$CACHE/toolchain.tar"; exit 1; }
    echo "$SHA  $CACHE/toolchain.tar" | sha256sum -c - >/dev/null ||
        { echo "SHA256 mismatch for $URL" >&2; rm -f "$CACHE/toolchain.tar"; exit 1; }
    tar -xf "$CACHE/toolchain.tar" -C "$CACHE"
    rm -f "$CACHE/toolchain.tar"
    [ -x "$DIR/relocate-sdk.sh" ] && "$DIR/relocate-sdk.sh" >/dev/null 2>&1 || true
fi
GCC=$(ls "$DIR"/bin/*-linux-musl*-gcc | head -1)
export STAGING_DIR="$DIR"     # OpenWrt's gcc wrapper warns without it
CROSS=${GCC%gcc}

# Objects of another toolchain are never rebuilt by make: a build directory per target
BUILD=$ROOT/build/musl-$TARGET
rm -rf "$BUILD"
mkdir -p "$BUILD" "$OUT"
make -s -C "$ROOT/src" BUILD_DIR="$BUILD" CROSS_COMPILE="$CROSS" LDLIBS=-static all
BIN=$OUT/camex-$TARGET
cp "$BUILD/camex" "$BIN"

# A static binary has no program interpreter (ld-linux*.so / ld-musl*.so)
if "${CROSS}readelf" -l "$BIN" | grep -q 'Requesting program interpreter'; then
    echo "$BIN is dynamically linked" >&2; exit 1
fi
echo "camex-$TARGET: $(wc -c < "$BIN") bytes, static"

if [ "${QEMU:-0}" = 1 ]; then
    # tests/Makefile builds in tests/: build there, then move the binaries away
    TESTS=$(make -s -C "$ROOT/tests" -p -n 2>/dev/null | sed -n 's/^TESTS = //p' | head -1)
    make -s -C "$ROOT/tests" clean
    make -s -C "$ROOT/tests" CC="${CROSS}gcc" EXTRA_CFLAGS=-static all
    for t in $TESTS; do mv "$ROOT/tests/$t" "$BUILD/$t"; done
    for t in $TESTS; do
        echo "== $QEMU_BIN $t"
        # no pipefail in sh: keep the test's own exit status, not that of tail
        out=$(cd "$ROOT/tests" && "$QEMU_BIN" "$BUILD/$t" 2>&1) ||
            { echo "$out"; echo "$t failed under $QEMU_BIN" >&2; exit 1; }
        echo "$out" | tail -1
    done
    "$QEMU_BIN" "$BIN" --version
fi
