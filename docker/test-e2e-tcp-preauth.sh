#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-e2e-tcp-preauth.sh — regression test for issue #9
#
# An attacker opens many TCP connections to an encrypted TCP server and
# keeps sending frames that do not decrypt. A legitimate client that
# connects during the attack must still register and reach the server.
# Before the fix every connection took a client slot at accept() and any
# frame refreshed it, so CAMEX_MAX_CLIENTS (256) connections locked
# legitimate clients out for as long as the attack lasted.
# Also: on a plaintext server, unregistered connections that keep sending
# keepalives are still closed after the pre-auth timeout.
#
# Usage: ./test-e2e-tcp-preauth.sh
#   CAMEX_BINARY=path/to/camex  binary to test (default: ../camex)
#   KEEP=1                      keep containers for debugging
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/camex}"
IMAGE="camex:e2e-preauth"
P="camex-preauth-$$"
NET_NAME="$P-net"
SRV="$P-srv"
CLI="$P-cli"
BUILD_CTX="$(mktemp -d)"
PSK="preauth-test"
ATTACKERS=10     # containers, i.e. source addresses
CONNS=30         # connections per attacker (300 > 256 slots)
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
cat > "$BUILD_CTX/Dockerfile" <<'EOF'
FROM ubuntu:24.04
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
        iputils-ping python3-minimal >/dev/null && rm -rf /var/lib/apt/lists/*
COPY camex /usr/local/bin/camex
COPY flood.py /flood.py
RUN printf '#!/bin/sh\nmkdir -p /dev/net\n[ -e /dev/net/tun ] || mknod /dev/net/tun c 10 200\nexec /usr/local/bin/camex "$@"\n' \
        > /usr/local/bin/camex-run && chmod +x /usr/local/bin/camex-run
ENTRYPOINT ["/usr/local/bin/camex-run"]
EOF
# flood.py <host> <conns> [keepalive]: keep <conns> TCP connections open and
# send a well-formed frame on each one every second: a "CX2P" DATA header
# with junk, or a plaintext KEEPALIVE; reconnect whatever the server closes.
cat > "$BUILD_CTX/flood.py" <<'EOF'
import os, socket, sys, time
host, n = sys.argv[1], int(sys.argv[2])
if len(sys.argv) > 3 and sys.argv[3] == "keepalive":
    frame = b"CX2P\x04"
else:
    frame = b"CX2P\x02" + os.urandom(60)
frame = len(frame).to_bytes(2, "big") + frame
conns = []
while True:
    while len(conns) < n:
        try:
            s = socket.create_connection((host, 5800), timeout=1)
            conns.append(s)
        except OSError:
            break
    alive = []
    for s in conns:
        try:
            s.sendall(frame)
            alive.append(s)
        except OSError:
            s.close()
    conns = alive
    time.sleep(1)
EOF
docker build -q -t "$IMAGE" "$BUILD_CTX" >/dev/null
docker network create "$NET_NAME" >/dev/null
set +e   # checks below report failures themselves

echo "== start server, flood it with $((ATTACKERS * CONNS)) unauthenticated connections"
docker run -d --name "$SRV" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    "$IMAGE" --mode server --port 5800 --transport tcp --local-cidr 10.200.0.1/24 \
    --encrypt --psk "$PSK" >/dev/null
sleep 1
for a in $(seq 1 "$ATTACKERS"); do
    docker run -d --name "$P-atk$a" --network "$NET_NAME" --entrypoint python3 \
        "$IMAGE" /flood.py "$SRV" "$CONNS" >/dev/null
done
sleep 8

echo "== a legitimate client connects during the attack"
docker run -d --name "$CLI" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    "$IMAGE" --mode client --transport tcp --local-cidr 10.200.0.2/24 \
    --gateway-ip 10.200.0.1 --server-host "$SRV" --port 5800 \
    --encrypt --psk "$PSK" >/dev/null
reached=1
for i in $(seq 1 40); do
    if docker exec "$CLI" ping -c1 -W1 10.200.0.1 >/dev/null 2>&1; then
        reached=0; echo "   reachable after ${i}s"; break
    fi
    sleep 1
done
check $reached "the client registers and reaches the server during the attack"
sleep 12
docker exec "$CLI" ping -c3 -W2 10.200.0.1 >/dev/null 2>&1
check $? "the client stays connected while the attack goes on"
docker logs "$SRV" 2>&1 | grep -qE "no REGISTER within|Closed unregistered TCP connection"
check $? "unregistered connections are limited (closed or timed out)"
! docker logs "$SRV" 2>&1 | grep -q "No free slots"
check $? "the server never ran out of client slots"

echo "== plaintext server: unregistered connections that send keepalives"
docker rm -f $(docker ps -aq --filter "name=^$P-") >/dev/null 2>&1
docker run -d --name "$SRV" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    "$IMAGE" --mode server --port 5800 --transport tcp --local-cidr 10.200.0.1/24 >/dev/null
sleep 1
docker run -d --name "$P-ka" --network "$NET_NAME" --entrypoint python3 \
    "$IMAGE" /flood.py "$SRV" 4 keepalive >/dev/null
sleep 14
docker logs "$SRV" 2>&1 | grep -q "no REGISTER within"
check $? "keepalives do not keep an unregistered connection open"

if [ "$FAILS" != 0 ]; then
    echo "--- server log (tail) ---"
    docker logs "$SRV" 2>&1 | tail -15
    echo "FAIL: $FAILS check(s) failed"
    exit 2
fi
echo "PASS: unauthenticated TCP connections cannot lock out a client"
