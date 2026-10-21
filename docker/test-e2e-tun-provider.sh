#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-e2e-tun-provider.sh — native side of the Android TUN path (#18, #19)
#
# The Android app creates the VPN interface through a provider callback
# once camex has the tunnel parameters from the server's CONFIG. This test
# runs camex_main() with a Linux provider (tests/android/test_tun_provider.c)
# against a real server and checks that the provider gets the CONFIG
# address, MTU and routes, that camex does not try to add routes itself,
# and that traffic flows in both directions (including server-initiated
# traffic, which stalled on a blocking TUN fd).
#
# Usage: ./test-e2e-tun-provider.sh
#   KEEP=1  keep containers for debugging
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
IMAGE="camex:e2e-provider"
P="camex-prov-$$"
SRV="$P-srv"
CLI="$P-cli"
BUILD_CTX="$(mktemp -d)"

cleanup() {
    docker rm -f "$SRV" "$CLI" >/dev/null 2>&1 || true
    docker network rm "$P-net" >/dev/null 2>&1 || true
    rm -rf "$BUILD_CTX"
}
[ -n "$KEEP" ] || trap cleanup EXIT INT TERM

fail() {
    echo "FAIL: $*"
    echo "--- client log ---"; docker logs "$CLI" 2>&1 | tail -20
    exit 2
}

# Build camex and the provider test program inside the image
mkdir -p "$BUILD_CTX/src" "$BUILD_CTX/test"
cp "$REPO_DIR"/src/*.c "$REPO_DIR"/src/*.h "$BUILD_CTX/src/"
cp "$REPO_DIR/tests/android/test_tun_provider.c" "$BUILD_CTX/test/"
cat > "$BUILD_CTX/Dockerfile" <<'DOCKERFILE'
FROM ubuntu:24.04
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
        gcc libc6-dev iproute2 iputils-ping >/dev/null && rm -rf /var/lib/apt/lists/*
COPY src /b/src
COPY test /b/test
RUN cd /b/src && CF="-std=c99 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -O1 -I." && \
    for f in camex monocypher util log crypto noise keys session tun packet net proto config client server slotidx wintun; do \
        gcc $CF -c $f.c -o /b/$f.o; done && \
    gcc $CF -Dmain=camex_program_main -c main.c -o /b/main.o && \
    gcc $CF -c ../test/test_tun_provider.c -o /b/t.o && \
    gcc -o /usr/local/bin/camex-provider /b/*.o && \
    gcc $CF -o /usr/local/bin/camex camex.c monocypher.c util.c log.c crypto.c noise.c keys.c session.c tun.c packet.c \
        net.c proto.c config.c client.c server.c slotidx.c wintun.c main.c
RUN printf '#!/bin/sh\nmkdir -p /dev/net\n[ -e /dev/net/tun ] || mknod /dev/net/tun c 10 200\nexec "$@"\n' \
        > /usr/local/bin/with-tun && chmod +x /usr/local/bin/with-tun
ENTRYPOINT ["/usr/local/bin/with-tun"]
DOCKERFILE
mkdir -p "$BUILD_CTX/etc-camex"
cat > "$BUILD_CTX/etc-camex/camex.conf" <<'CONF'
[server]
[client PHONE]
local_cidr=10.200.0.7/24
gateway_ip=10.200.0.1
mtu=1400
route_cidr=192.168.77.0/24
CONF
docker build -q -t "$IMAGE" "$BUILD_CTX" >/dev/null
# Protocol v2 server key, made by the camex just built in the image
KEYS=$(docker run --rm "$IMAGE" camex --genkey)
printf '%s\n' "$KEYS" | grep '^server_key=' > "$BUILD_CTX/etc-camex/server.key"
SERVER_PUBKEY=$(printf '%s\n' "$KEYS" | sed -n 's/^public_key=//p')
docker network create "$P-net" >/dev/null

docker create --name "$SRV" --network "$P-net" --cap-add NET_ADMIN --cap-add MKNOD \
    "$IMAGE" camex --mode server --port 5800 --local-cidr 10.200.0.1/24 \
    --server-key-file /etc/camex/server.key --psk k \
    --config /etc/camex/camex.conf >/dev/null
docker cp "$BUILD_CTX/etc-camex" "$SRV:/etc/camex" >/dev/null
docker start "$SRV" >/dev/null
sleep 1
docker run -d --name "$CLI" --network "$P-net" --cap-add NET_ADMIN --cap-add MKNOD \
    "$IMAGE" camex-provider --mode client --auto --name PHONE \
    --server-host "$SRV" --port 5800 --server-pubkey "$SERVER_PUBKEY" \
    --psk k >/dev/null

ok=0
for _ in $(seq 1 20); do
    if docker exec "$CLI" ping -c1 -W1 10.200.0.1 >/dev/null 2>&1; then ok=1; break; fi
    sleep 1
done
[ "$ok" = 1 ] || fail "client never reached the server"

docker logs "$CLI" 2>&1 | grep -q "PROVIDER ip=10.200.0.7 prefix=24 mtu=1400 routes=1 192.168.77.0/24" ||
    fail "provider did not get the CONFIG address, MTU and routes"
echo "   ok: provider got 10.200.0.7/24, mtu 1400, route 192.168.77.0/24 from CONFIG"
! docker logs "$CLI" 2>&1 | grep -q "PROVIDER route failed" ||
    fail "the provider could not install a CONFIG route"
docker exec "$CLI" ip route show 192.168.77.0/24 | grep -q "dev prov0" ||
    fail "route 192.168.77.0/24 is not on the provider's interface"
echo "   ok: route 192.168.77.0/24 is installed on the provider's interface"
! docker logs "$CLI" 2>&1 | grep -q "Route setup failed" ||
    fail "camex tried to add routes for an external TUN"
echo "   ok: camex left routing to the provider"

# Server-initiated traffic only: with a blocking TUN fd the client would
# stall in read() after the first packet and stop handling the socket.
docker exec "$SRV" ping -c 20 -i 0.2 -W1 10.200.0.7 >/dev/null 2>&1 ||
    fail "server-initiated traffic to the client stalls"
echo "   ok: server-initiated traffic reaches the client (20 of 20)"

echo "PASS: TUN provider gets the CONFIG parameters and the tunnel works"
