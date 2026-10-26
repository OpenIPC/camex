#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-e2e-persistent-tun.sh — a persistent TUN interface is left clean (#99)
#
# With camex.ko (--tun-dev /dev/camex) the interface belongs to the module
# and outlives the client. Here a persistent tun interface named camex
# (ip tuntap add) stands in for it: /dev/camex is the tun clone device,
# so the client sets up the interface by name as it does with camex.ko.
# The fd is not attached to the interface (no TUNSETIFF on an override
# path, as with camex.ko): this test covers the interface's life cycle
# only, not packets. Traffic over the real camex.ko: the QEMU test
# (docker/test-kmod-vm.sh with CAMEX_BINARY) and the device test.
#
# 1. The client brings camex up with its address and the tunnel route.
# 2. After SIGTERM camex is still there (it is not ours to delete), but
#    down, without the address and without a route through it.
# 3. A client started again on the same interface gets it back.
# 4. /dev/net/tun as before: the client's tun interface goes away with it.
#
# Usage: ./test-e2e-persistent-tun.sh
#   CAMEX_BINARY=path/to/camex  binary to test (default: ../camex)
#   KEEP=1                      keep containers for debugging
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/camex}"
IMAGE="camex:e2e-persistent-tun"
P="camex-ptun-$$"
NET_NAME="$P-net"
BUILD_CTX="$(mktemp -d)"
PSK="persistent-tun-test"
FAILS=0

cleanup() {
    docker ps -a --format '{{.Names}}' | grep "^$P-" | xargs -r docker rm -f >/dev/null 2>&1 || true
    docker network rm "$NET_NAME" >/dev/null 2>&1 || true
    rm -rf "$BUILD_CTX"
}
[ -n "$KEEP" ] || trap cleanup EXIT
# Ctrl+C or a cancelled job ends the script; EXIT then cleans up once (#125)
trap 'exit 130' INT
trap 'exit 143' TERM

check() {   # check <condition-exit-code> <message>
    if [ "$1" = 0 ]; then echo "   ok: $2"; else echo "   FAIL: $2"; FAILS=$((FAILS + 1)); fi
}

[ -x "$CAMEX_BINARY" ] || { echo "camex binary not found: $CAMEX_BINARY"; exit 1; }

cp "$CAMEX_BINARY" "$BUILD_CTX/camex"
. "$SCRIPT_DIR/lib-keys.sh"
gen_server_key "$BUILD_CTX" "$CAMEX_BINARY" || { echo "cannot create a server key"; exit 1; }
cat > "$BUILD_CTX/Dockerfile" <<'DOCKERFILE'
FROM ubuntu:24.04
# New clients right after the server starts: no start hold (#81)
ENV CAMEX_POOL_HOLD=0
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
        iproute2 procps >/dev/null && rm -rf /var/lib/apt/lists/*
COPY camex /usr/local/bin/camex
COPY server.key /etc/camex/server.key
RUN printf '#!/bin/sh\nmkdir -p /dev/net\n[ -e /dev/net/tun ] || mknod /dev/net/tun c 10 200\nexec /usr/local/bin/camex "$@"\n' \
        > /usr/local/bin/camex-run && chmod +x /usr/local/bin/camex-run
ENTRYPOINT ["/usr/local/bin/camex-run"]
DOCKERFILE
docker build -q -t "$IMAGE" "$BUILD_CTX" >/dev/null 2>&1
docker network create "$NET_NAME" >/dev/null
set +e   # checks below report failures themselves

docker run -d --name "$P-srv" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    "$IMAGE" --mode server --port 5800 --local-cidr 10.200.0.1/24 \
    $SERVER_KEY_ARGS --psk "$PSK" >/dev/null
# The client container idles; clients run in it with docker exec
docker run -d --name "$P-cli" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    --entrypoint /bin/sh "$IMAGE" -c 'mkdir -p /dev/net
        [ -e /dev/net/tun ] || mknod /dev/net/tun c 10 200
        mknod /dev/camex c 10 200
        ip tuntap add dev camex mode tun
        exec sleep 3600' >/dev/null
sleep 1

cli() { docker exec "$P-cli" sh -c "$1"; }

start_client() {    # start_client <name> [camex args...]: in the background
    local name="$1"; shift
    docker exec -d "$P-cli" sh -c "exec camex --mode client --auto --name $name \
        --server-host $P-srv --port 5800 --server-pubkey $SERVER_PUBKEY --psk $PSK \
        --keepalive 2 $* > /tmp/$name.log 2>&1"
}

wait_addr() {       # wait_addr <ifname>: until it has an IPv4 address
    local _
    for _ in $(seq 1 20); do
        cli "ip -4 addr show dev $1 2>/dev/null | grep -q inet" && return 0
        sleep 1
    done
    return 1
}

stop_client() {     # stop_client <name>: SIGTERM, wait until it is gone
    local _
    # pkill must find the client: one that is already gone did not get
    # the signal, and its clean-up would not be tested
    cli "pkill -TERM -f '^camex .*--name $1 '" || return 1
    for _ in $(seq 1 10); do
        cli "pgrep -f '^camex .*--name $1 ' >/dev/null" || return 0
        sleep 1
    done
    return 1
}

clean() {           # clean <ifname>: there, down, no address, no route
    cli "ip link show $1 >/dev/null 2>&1" &&
        ! cli "ip link show $1 | grep -q '[<,]UP[,>]'" &&
        ! cli "ip -4 addr show dev $1 | grep -q inet" &&
        ! cli "ip route | grep -q 'dev $1 '"
}

echo "== 1. a client on the persistent interface"
start_client CAM-P --tun-dev /dev/camex
wait_addr camex
check $? "camex gets an address ($(cli 'ip -4 -br addr show dev camex' | awk '{print $3}'))"
cli "ip link show camex | grep -q '[<,]UP[,>]'"
check $? "camex is up"
cli "ip route | grep -q '^10.200.0.0/24 dev camex '"
check $? "the tunnel route goes through camex"

echo "== 2. SIGTERM"
stop_client CAM-P
check $? "the client exits"
clean camex
check $? "camex stays, but down, without the address and the route"
cli "grep -q 'Interface camex down, address removed' /tmp/CAM-P.log"
check $? "the client logs it"

echo "== 3. a client again on the same interface"
start_client CAM-P --tun-dev /dev/camex
wait_addr camex
check $? "camex gets its address again"
stop_client CAM-P
clean camex
check $? "and is left clean again"

echo "== 4. /dev/net/tun as before"
start_client CAM-T
wait_addr tun0
check $? "the client's tun0 gets an address"
stop_client CAM-T
check $? "the client exits"
! cli "ip link show tun0 >/dev/null 2>&1"
check $? "tun0 is gone with the client"

if [ "$FAILS" -ne 0 ]; then
    echo "--- logs"; cli 'tail -n 20 /tmp/*.log'
    echo "FAIL: $FAILS check(s) failed"
    exit 1
fi
echo "PASS: a persistent TUN interface is left clean"
