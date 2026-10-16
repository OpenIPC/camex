#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-e2e-bad-config.sh — a malformed CONFIG from the server is not
# applied (#12, #49)
#
# A fake server (tests/v2tool serve: a valid handshake, then any CONFIG
# text) sends an auto client CONFIGs a real server never would: shell
# characters in the gateway, a /33 address, an MTU of 70000, a bad route,
# another client's ID.
#
# 1. As the first CONFIG: the client applies nothing — no tunnel address,
#    no route — and does not run any of it.
# 2. After a valid first CONFIG (the client is up), the same CONFIGs come
#    with the next handshake (the fake server answers no keepalives, so
#    the client reconnects): the client keeps its address and routes.
#
# Usage: ./test-e2e-bad-config.sh
#   CAMEX_BINARY=path/to/camex  binary to test (default: ../camex)
#   KEEP=1                      keep containers for debugging
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/camex}"
IMAGE="camex:e2e-bad-config"
P="camex-badcfg-$$"
NET_NAME="$P-net"
SRV="$P-srv"
CLI="$P-cli"
BUILD_CTX="$(mktemp -d)"
PSK="bad-config-test"
FAILS=0

cleanup() {
    docker ps -a --format '{{.Names}}' | grep "^$P-" | xargs -r docker rm -f >/dev/null 2>&1 || true
    docker network rm "$NET_NAME" >/dev/null 2>&1 || true
    rm -rf "$BUILD_CTX"
}
[ -n "$KEEP" ] || trap cleanup EXIT INT TERM

check() {   # check <condition-exit-code> <message>
    if [ "$1" = 0 ]; then echo "   ok: $2"; else echo "   FAIL: $2"; FAILS=$((FAILS + 1)); fi
}

[ -x "$CAMEX_BINARY" ] || { echo "camex binary not found: $CAMEX_BINARY"; exit 1; }

cp "$CAMEX_BINARY" "$BUILD_CTX/camex"
. "$SCRIPT_DIR/lib-keys.sh"
gen_server_key "$BUILD_CTX" "$CAMEX_BINARY" || { echo "cannot create a server key"; exit 1; }
build_v2tool "$BUILD_CTX" || { echo "cannot build tests/v2tool"; exit 1; }
SERVER_PRIV=$(sed -n 's/^server_key=//p' "$BUILD_CTX/server.key")
cat > "$BUILD_CTX/Dockerfile" <<'EOF'
FROM ubuntu:24.04
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
        iputils-ping iproute2 >/dev/null && rm -rf /var/lib/apt/lists/*
COPY camex /usr/local/bin/camex
COPY v2tool /usr/local/bin/v2tool
RUN printf '#!/bin/sh\nmkdir -p /dev/net\n[ -e /dev/net/tun ] || mknod /dev/net/tun c 10 200\nexec /usr/local/bin/camex "$@"\n' \
        > /usr/local/bin/camex-run && chmod +x /usr/local/bin/camex-run
ENTRYPOINT ["/usr/local/bin/camex-run"]
EOF
docker build -q -t "$IMAGE" "$BUILD_CTX" >/dev/null
docker network create "$NET_NAME" >/dev/null
set +e   # checks below report failures themselves

HEAD="CAMEX CONFIG VER=2 CLIENT_ID=CAM1"
GOOD="$HEAD MTU=1500 LOCAL_CIDR=10.200.0.5/24 GATEWAY_IP=10.200.0.1 ROUTE_CIDR=192.168.150.0/24"
BAD_NAMES=(gateway cidr mtu route client-id)
BAD=(
    "$HEAD MTU=1500 LOCAL_CIDR=10.200.0.9/24 GATEWAY_IP=10.200.0.1;touch\${IFS}/tmp/pwned ROUTE_CIDR=192.168.160.0/24"
    "$HEAD MTU=1500 LOCAL_CIDR=10.200.0.9/33 GATEWAY_IP=10.200.0.1 ROUTE_CIDR=192.168.160.0/24"
    "$HEAD MTU=70000 LOCAL_CIDR=10.200.0.9/24 GATEWAY_IP=10.200.0.1 ROUTE_CIDR=192.168.160.0/24"
    "$HEAD MTU=1500 LOCAL_CIDR=10.200.0.9/24 GATEWAY_IP=10.200.0.1 ROUTE_CIDR=192.168.160.0/24;touch\${IFS}/tmp/pwned"
    "CAMEX CONFIG VER=2 CLIENT_ID=OTHER MTU=1500 LOCAL_CIDR=10.200.0.9/24 GATEWAY_IP=10.200.0.1 ROUTE_CIDR=192.168.160.0/24"
)

# run <first CONFIG> <later CONFIG>: fake server and an auto client
run() {
    docker rm -f "$SRV" "$CLI" >/dev/null 2>&1
    docker run -d --name "$SRV" --network "$NET_NAME" --entrypoint /usr/local/bin/v2tool \
        "$IMAGE" serve 5800 "$SERVER_PRIV" "$PSK" "$1" "$2" >/dev/null
    sleep 1
    docker run -d --name "$CLI" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
        "$IMAGE" --mode client --auto --name CAM1 --server-host "$SRV" --port 5800 \
        --server-pubkey "$SERVER_PUBKEY" --psk "$PSK" \
        --keepalive 2 --server-timeout 5 >/dev/null
}

# state: the client's tunnel addresses and routes ("" if it has exited)
state() {
    docker exec "$CLI" sh -c 'ip -4 -o addr show; ip route; ls /tmp/pwned 2>/dev/null' 2>/dev/null
}

echo "== 1. a malformed first CONFIG is not applied"
for i in "${!BAD[@]}"; do
    run "${BAD[$i]}" "${BAD[$i]}"
    sleep 6
    s=$(state)
    answered=$(docker logs "$SRV" 2>&1 | grep -c "answered handshake")
    [ "$answered" -ge 1 ] && ! echo "$s" | grep -q "10.200.0.9\|192.168.160\|pwned"
    check $? "${BAD_NAMES[$i]}: no address, route or command from it (handshakes answered: $answered)"
done

echo "== 2. a malformed CONFIG on a later handshake keeps the working settings"
for i in "${!BAD[@]}"; do
    run "$GOOD" "${BAD[$i]}"
    up=1
    for _ in $(seq 1 15); do
        state | grep -q "10.200.0.5/24" && { up=0; break; }
        sleep 1
    done
    # no keepalive answers: the client reconnects every few seconds and
    # gets the malformed CONFIG each time
    later=1
    for _ in $(seq 1 20); do
        [ "$(docker logs "$SRV" 2>&1 | grep -c "answered handshake")" -ge 3 ] && { later=0; break; }
        sleep 1
    done
    s=$(state)
    [ "$up" = 0 ] && [ "$later" = 0 ] &&
        echo "$s" | grep -q "10.200.0.5/24" && echo "$s" | grep -q "^192.168.150.0/24" &&
        ! echo "$s" | grep -q "10.200.0.9\|192.168.160\|pwned" &&
        ! docker logs "$CLI" 2>&1 | grep -q "Applying the new tunnel configuration"
    check $? "${BAD_NAMES[$i]}: still 10.200.0.5 with its route after later handshakes"
done

if [ "$FAILS" != 0 ]; then
    echo "--- client log (tail) ---"
    docker logs "$CLI" 2>&1 | tail -15
    echo "--- fake server log (tail) ---"
    docker logs "$SRV" 2>&1 | tail -5
    echo "FAIL: $FAILS check(s) failed"
    exit 2
fi
echo "PASS: malformed CONFIGs are never applied, first or later"
