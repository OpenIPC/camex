#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-e2e-tcp-pending.sh — regression test for issue #8
#
# A TCP client that stops reading leaves unsent data in the server's
# pending-send buffer. When that client disconnects, the server must keep
# serving the other clients (no select() EBADF loop, no stale bytes).
#
# Scenario:
#   1. server (TCP, tunnel 10.200.0.1/24) + real client B (auto mode)
#   2. client A (tests/v2tool) registers as 10.200.0.50 and never reads
#   3. B floods 10.200.0.50 so the server's send to A blocks
#   4. A disconnects
#   5. B must still reach the server through the tunnel
#
# Usage: ./test-e2e-tcp-pending.sh
#   CAMEX_BINARY=path/to/camex  binary to test (default: ../camex)
#   KEEP=1                      keep containers for debugging
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/camex}"
IMAGE="camex:e2e-net"
NET_NAME="camex-pend-$$"
SRV="camex-pend-srv-$$"
CLB="camex-pend-b-$$"
CLA="camex-pend-a-$$"
BUILD_CTX="$(mktemp -d)"
PSK="pending-test"

cleanup() {
    docker rm -f "$SRV" "$CLB" "$CLA" >/dev/null 2>&1 || true
    docker network rm "$NET_NAME" >/dev/null 2>&1 || true
    rm -rf "$BUILD_CTX"
}
[ -n "$KEEP" ] || trap cleanup EXIT INT TERM

fail() {
    echo "FAIL: $*"
    echo "--- server log (tail) ---"
    docker logs "$SRV" 2>&1 | tail -20 || true
    exit 2
}

[ -x "$CAMEX_BINARY" ] || { echo "camex binary not found: $CAMEX_BINARY"; exit 1; }

# Image with ping and python3; /dev/net/tun is created inside the container
# so the test also works on hosts without a /dev/net/tun node.
cp "$CAMEX_BINARY" "$BUILD_CTX/camex"
. "$SCRIPT_DIR/lib-keys.sh"
gen_server_key "$BUILD_CTX" "$CAMEX_BINARY" || { echo "cannot create a server key"; exit 1; }
build_v2tool "$BUILD_CTX" || { echo "cannot build tests/v2tool"; exit 1; }
cat > "$BUILD_CTX/Dockerfile" <<'EOF'
FROM ubuntu:24.04
# New clients right after the server starts: no start hold (#81)
ENV CAMEX_POOL_HOLD=0
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
        iputils-ping python3-minimal >/dev/null && rm -rf /var/lib/apt/lists/*
COPY camex /usr/local/bin/camex
COPY server.key /etc/camex/server.key
COPY v2tool /usr/local/bin/v2tool
RUN printf '#!/bin/sh\nmkdir -p /dev/net\n[ -e /dev/net/tun ] || mknod /dev/net/tun c 10 200\nexec /usr/local/bin/camex "$@"\n' \
        > /usr/local/bin/camex-run && chmod +x /usr/local/bin/camex-run
ENTRYPOINT ["/usr/local/bin/camex-run"]
EOF
docker build -q -t "$IMAGE" "$BUILD_CTX" >/dev/null

docker network create "$NET_NAME" >/dev/null

echo "== start server and client B"
docker run -d --name "$SRV" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    "$IMAGE" --mode server --port 5800 --transport tcp \
    --local-cidr 10.200.0.1/24 $SERVER_KEY_ARGS --psk "$PSK" >/dev/null
sleep 1
docker run -d --name "$CLB" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    "$IMAGE" --mode client --auto --name B01 --server-host "$SRV" --port 5800 \
    --transport tcp --server-pubkey "$SERVER_PUBKEY" --psk "$PSK" >/dev/null

ok=0
for _ in $(seq 1 20); do
    if docker exec "$CLB" ping -c1 -W1 10.200.0.1 >/dev/null 2>&1; then
        ok=1; break
    fi
    sleep 1
done
[ "$ok" = 1 ] || fail "client B never reached the server through the tunnel"
echo "   B reaches 10.200.0.1"

echo "== start slow client A (registers 10.200.0.50, never reads)"
docker run -d --name "$CLA" --network "$NET_NAME" \
    -e HOLD=1 -e GO_FILE=/tmp/go -e RCVBUF=4096 \
    --entrypoint /usr/local/bin/v2tool "$IMAGE" \
    "$SRV" 5800 "$SERVER_PUBKEY" "$PSK" tcp manual:10.200.0.50 >/dev/null
sleep 2

echo "== flood 10.200.0.50 from B until the server's send to A blocks"
docker exec "$CLB" python3 -c '
import socket, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
d = b"x" * 1400
t = time.time()
while time.time() - t < 3:
    try:
        s.sendto(d, ("10.200.0.50", 9))
    except OSError:
        pass
'

echo "== the server's socket to A must hold unsent data"
# Precondition for the rest of the test: A registered (otherwise the flood
# never reaches its socket) and the server's send queue to A is not empty.
A_IP=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$CLA")
txq=$(docker exec "$SRV" python3 -c '
import socket, struct, sys
want = "%08X" % struct.unpack("<I", socket.inet_aton(sys.argv[1]))[0]
best = 0
for line in open("/proc/net/tcp").readlines()[1:]:
    f = line.split()
    local, remote, txrx = f[1], f[2], f[4]
    if local.endswith(":16A8") and remote.split(":")[0] == want:
        best = max(best, int(txrx.split(":")[0], 16))
print(best)
' "$A_IP")
[ "${txq:-0}" -gt 0 ] || \
    fail "no pending data on the server's socket to A (send queue: ${txq:-none}); the scenario did not run"
echo "   server send queue to A: $txq bytes"

echo "== B must reach the server while A is blocked"
docker exec "$CLB" ping -c3 -W1 10.200.0.1 >/dev/null 2>&1 || \
    fail "head-of-line blocking: client B is starved while A is not reading"

echo "== A closes gracefully (FIN) with data still pending on the server"
docker exec "$CLA" touch /tmp/go
sleep 3

echo "== B must still reach the server"
ok=0
for _ in $(seq 1 10); do
    if docker exec "$CLB" ping -c1 -W1 10.200.0.1 >/dev/null 2>&1; then
        ok=1; break
    fi
    sleep 1
done
[ "$ok" = 1 ] || fail "server stopped serving client B after A disconnected"

ebadf=$(docker logs "$SRV" 2>&1 | grep -c "select" || true)
[ "$ebadf" -lt 5 ] || fail "server logged $ebadf select() errors"

echo "PASS: no head-of-line blocking; server keeps serving after a client with pending data disconnects"
