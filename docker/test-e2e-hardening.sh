#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-e2e-hardening.sh — regression tests for issue #16
#
#   L3: after a config reload (SIGHUP) that changes a client's key, the
#       session registered with the old key ends; before the fix it went
#       on until the client disconnected.
#   L7: a TCP client whose server does not answer SYNs times out its
#       connect() after a few seconds and retries; before the fix a
#       blocking connect() stalled the client for minutes.
#   L1: a flood of undecryptable packets produces a bounded number of
#       log lines.
#
# Usage: ./test-e2e-hardening.sh
#   CAMEX_BINARY=path/to/camex  binary to test (default: ../camex)
#   KEEP=1                      keep containers for debugging
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/camex}"
IMAGE="camex:e2e-hardening"
P="camex-hard-$$"
NET_NAME="$P-net"
SRV="$P-srv"
BUILD_CTX="$(mktemp -d)"
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
RUN printf '#!/bin/sh\nmkdir -p /dev/net\n[ -e /dev/net/tun ] || mknod /dev/net/tun c 10 200\nexec /usr/local/bin/camex "$@"\n' \
        > /usr/local/bin/camex-run && chmod +x /usr/local/bin/camex-run
ENTRYPOINT ["/usr/local/bin/camex-run"]
EOF
docker build -q -t "$IMAGE" "$BUILD_CTX" >/dev/null
docker network create "$NET_NAME" >/dev/null
set +e   # checks below report failures themselves

conf() {    # conf <CAMA key>: write the server config
    printf '[server]\n[client CAMA]\nlocal_cidr=10.200.0.10/24\ngateway_ip=10.200.0.1\npsk=%s\n' "$1" \
        > "$BUILD_CTX/camex.conf"
    docker cp "$BUILD_CTX/camex.conf" "$SRV:/etc/camex.conf" >/dev/null
}

echo "== L3: a config reload that changes a client's key ends its session"
docker create --name "$SRV" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    "$IMAGE" --mode server --port 5800 --local-cidr 10.200.0.1/24 \
    --encrypt --psk global-key --config /etc/camex.conf >/dev/null
conf key-a
docker start "$SRV" >/dev/null
sleep 1
docker run -d --name "$P-cama" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    "$IMAGE" --mode client --auto --name CAMA --server-host "$SRV" --port 5800 \
    --encrypt --psk key-a >/dev/null
up=1
for _ in $(seq 1 20); do
    docker exec "$P-cama" ping -c1 -W1 10.200.0.1 >/dev/null 2>&1 && { up=0; break; }
    sleep 1
done
check $up "CAMA reaches the server with its key"
conf key-new
docker kill -s HUP "$SRV" >/dev/null
sleep 3
docker logs "$SRV" 2>&1 | grep -q "Ended session of client CAMA"
check $? "the server ends CAMA's session after the reload"
! docker exec "$P-cama" ping -c2 -W1 10.200.0.1 >/dev/null 2>&1
check $? "CAMA's old key no longer reaches the server"

echo "== L1: a flood of undecryptable packets is logged a bounded number of times"
before=$(docker logs "$SRV" 2>&1 | wc -l)
docker run --rm --network "$NET_NAME" --entrypoint python3 "$IMAGE" -c '
import os, socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
for _ in range(2000):
    s.sendto(b"CX2P\x02" + os.urandom(60), (sys.argv[1], 5800))
' "$SRV"
sleep 2
lines=$(( $(docker logs "$SRV" 2>&1 | wc -l) - before ))
[ "$lines" -lt 100 ]; check $? "2000 bad packets produce fewer than 100 log lines (got $lines)"

echo "== L7: TCP connect to a server that does not answer times out"
# 10.200.0.0/24 is not routed on the docker network: SYNs go unanswered
docker run -d --name "$P-tcp" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    "$IMAGE" --mode client --transport tcp --local-cidr 10.201.0.2/24 \
    --gateway-ip 10.201.0.1 --server-host 192.0.2.1 --port 5800 >/dev/null
sleep 20
timeouts=$(docker logs "$P-tcp" 2>&1 | grep -c "connect(TCP): timed out")
[ "$timeouts" -ge 2 ]; check $? "connect() times out and is retried (timeouts in 20 s: $timeouts)"
start=$(date +%s)
docker stop -t 10 "$P-tcp" >/dev/null
took=$(( $(date +%s) - start ))
[ "$took" -le 7 ]; check $? "the client stops promptly while connecting (took ${took}s)"

if [ "$FAILS" != 0 ]; then
    echo "--- server log (tail) ---"
    docker logs "$SRV" 2>&1 | tail -10
    echo "FAIL: $FAILS check(s) failed"
    exit 2
fi
echo "PASS: reload revokes keys, log floods are bounded, TCP connect times out"
