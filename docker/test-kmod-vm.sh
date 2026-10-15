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
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
IMAGE="${1:-ubuntu:24.04}"

c=$(docker create -e SMP="${SMP:-2}" "$IMAGE" sleep 3600)
trap 'docker rm -f "$c" >/dev/null 2>&1 || true' EXIT INT TERM
docker start "$c" >/dev/null
docker exec "$c" mkdir -p /k/src
for f in camex-k.c Kbuild version.h; do
    docker cp "$REPO_DIR/src/$f" "$c:/k/src/$f" >/dev/null
done
docker cp "$REPO_DIR/tests/kmod" "$c:/k/kmod" >/dev/null
docker exec "$c" sh /k/kmod/run-vm.sh
