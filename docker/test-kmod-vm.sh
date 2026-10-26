#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-kmod-vm.sh — load camex.ko in a QEMU guest and test it at runtime
#
# No root and no host kernel headers needed: everything runs inside a
# container that boots the distro kernel in QEMU (TCG, or KVM if
# /dev/kvm is passed through). Takes a few minutes without KVM.
#
# Usage: ./test-kmod-vm.sh [image]     default image: ubuntu:24.04
#   ubuntu:24.04 -> kernel 6.8   (netif_rx() path)
#   ubuntu:18.04 -> kernel 4.15  (netif_rx_ni() path)
#   SMP=<n>      number of guest vCPUs (default 2)
#   CAMEX_BINARY=<static camex>  also run the camex client in the guest over
#                /dev/camex against a server in the container (e.g.
#                build-musl/camex-x86_64 from ci/build-musl.sh x86_64)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
IMAGE="${1:-ubuntu:24.04}"

KVM_DEV=
[ -w /dev/kvm ] && KVM_DEV="--device /dev/kvm"
# NET_ADMIN and MKNOD: the camex server's tun device in the container
c=$(docker create -e SMP="${SMP:-2}" --cap-add NET_ADMIN --cap-add MKNOD $KVM_DEV \
    "$IMAGE" sleep 3600)
trap 'docker rm -f "$c" >/dev/null 2>&1 || true' EXIT
# Ctrl+C or a cancelled job ends the script; EXIT then cleans up once (#127)
trap 'exit 130' INT
trap 'exit 143' TERM
docker start "$c" >/dev/null
docker exec "$c" mkdir -p /k/src
for f in camex-k.c Kbuild version.h; do
    docker cp "$REPO_DIR/src/$f" "$c:/k/src/$f" >/dev/null
done
docker cp "$REPO_DIR/tests/kmod" "$c:/k/kmod" >/dev/null
if [ -n "${CAMEX_BINARY:-}" ]; then
    file "$CAMEX_BINARY" | grep -q "statically linked" ||
        { echo "CAMEX_BINARY must be a static build: $CAMEX_BINARY"; exit 1; }
    docker cp "$CAMEX_BINARY" "$c:/k/camex" >/dev/null
fi
docker exec "$c" sh /k/kmod/run-vm.sh
