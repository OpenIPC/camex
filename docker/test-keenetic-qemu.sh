#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-keenetic-qemu.sh — run a cross-built camex under QEMU user mode
# against a native camex, in both roles (issue #33).
#
# The target binary talks to an x86_64 peer, so a byte-order bug in the
# protocol code (big-endian MIPS) shows up as a failed handshake or ping.
#
# Usage: test-keenetic-qemu.sh <qemu-arch> <target-binary> <native-binary>
#   qemu-arch: mips | mipsel | aarch64
#
# Scenarios:
#   1. --version under QEMU
#   2. native server (UDP, encrypted) + target client (auto mode)
#   3. target server (TCP, encrypted) + native client (auto mode)
#

set -u

QARCH="$1"
TARGET_BIN="$2"
NATIVE_BIN="$3"

P="camex-kn-$QARCH-$$"
IMAGE="camex:keenetic-qemu"
PSK="keenetic-ci"
BUILD_CTX=$(mktemp -d)
FAILED=0

cleanup() {
    docker rm -f "$P-srv" "$P-cli" >/dev/null 2>&1
    docker network rm "$P-net" >/dev/null 2>&1
    rm -rf "$BUILD_CTX"
}
trap cleanup EXIT INT TERM

check() {
    if [ "$1" -eq 0 ]; then
        echo "   PASS: $2"
    else
        echo "   FAIL: $2"
        FAILED=1
    fi
}

dump_logs() {
    for c in "$P-srv" "$P-cli"; do
        echo "---- $c"
        docker logs "$c" 2>&1 | tail -20
    done
}

cp "$TARGET_BIN" "$BUILD_CTX/camex-target"
cp "$NATIVE_BIN" "$BUILD_CTX/camex-native"
cat > "$BUILD_CTX/camex.conf" <<'EOF'
[client KN01]
local_cidr=10.201.0.2/24
gateway_ip=10.201.0.1
route_cidr=192.168.177.0/24
EOF
cat > "$BUILD_CTX/Dockerfile" <<EOF
FROM ubuntu:24.04
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
        qemu-user-static iproute2 iputils-ping >/dev/null && \
    rm -rf /var/lib/apt/lists/*
COPY camex-target camex-native /usr/local/bin/
COPY camex.conf /etc/camex/camex.conf
# target binary always runs through QEMU explicitly (no binfmt needed)
RUN printf '#!/bin/sh\nexec qemu-$QARCH-static /usr/local/bin/camex-target "\$@"\n' \
        > /usr/local/bin/camex-q && chmod +x /usr/local/bin/camex-q
EOF
docker build -q -t "$IMAGE" "$BUILD_CTX" >/dev/null || { echo "image build failed"; exit 1; }
docker network create "$P-net" >/dev/null || exit 1

run() {   # run <name> <binary> <camex args...>
    local name="$1" bin="$2"
    shift 2
    docker run -d --name "$name" --network "$P-net" \
        --cap-add NET_ADMIN --device /dev/net/tun:/dev/net/tun \
        --security-opt seccomp=unconfined \
        --entrypoint "$bin" "$IMAGE" "$@" >/dev/null
}

ping_ok() {   # ping_ok <container> <addr>
    local i
    for i in $(seq 1 20); do
        docker exec "$1" ping -c1 -W1 "$2" >/dev/null 2>&1 && return 0
        sleep 1
    done
    return 1
}

echo "== 1. --version under qemu-$QARCH"
docker run --rm --entrypoint camex-q "$IMAGE" --version
check $? "target binary starts under QEMU"

echo "== 2. native server (UDP, encrypted) + $QARCH client"
run "$P-srv" camex-native --mode server --port 5800 --local-cidr 10.201.0.1/24 \
    --config /etc/camex/camex.conf --encrypt --psk "$PSK"
sleep 1
run "$P-cli" camex-q --mode client --auto --name KN01 --server-host "$P-srv" \
    --port 5800 --encrypt --psk "$PSK"
ping_ok "$P-cli" 10.201.0.1
r=$?
check $r "$QARCH client reaches the server through the tunnel"
docker exec "$P-cli" ip route | grep -q "192.168.177.0/24"
check $? "$QARCH client installed the CONFIG route"
[ $r -eq 0 ] || dump_logs
docker rm -f "$P-srv" "$P-cli" >/dev/null 2>&1

echo "== 3. $QARCH server (TCP, encrypted) + native client"
run "$P-srv" camex-q --mode server --port 5800 --transport tcp \
    --local-cidr 10.201.0.1/24 --config /etc/camex/camex.conf --encrypt --psk "$PSK"
sleep 2
run "$P-cli" camex-native --mode client --auto --name KN01 --server-host "$P-srv" \
    --port 5800 --transport tcp --encrypt --psk "$PSK"
ping_ok "$P-cli" 10.201.0.1
r=$?
check $r "native client reaches the $QARCH server through the tunnel"
[ $r -eq 0 ] || dump_logs

if [ "$FAILED" -ne 0 ]; then
    echo "FAIL: camex under qemu-$QARCH"
    exit 1
fi
echo "PASS: camex under qemu-$QARCH interoperates with x86_64 in both roles"
