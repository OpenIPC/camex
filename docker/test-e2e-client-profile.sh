#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-e2e-client-profile.sh — the client profile (#204): camex built with
# PROFILE=client, without the server
#
#  1. --version names what it has: client, l2, mesh, and no server
#  2. It refuses --mode server and --format nginx-map / nginx-index, and
#     --help shows no server options
#  3. It connects to a server of the default (full) build over UDP and
#     over TCP: pings go both ways
#
# Usage: ./test-e2e-client-profile.sh
#   CAMEX_BINARY=path/to/camex                the server (default: ../camex)
#   CAMEX_CLIENT_BINARY=path/to/camex-client  the client (default: built
#                                             here with PROFILE=client)
#   KEEP=1                                    keep containers for debugging
#

set -e
export LC_ALL=C

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/camex}"
IMAGE="camex:e2e-client-profile"
P="camex-cprof-$$"
NET_NAME="$P-net"
SRV="$P-srv"
CLI="$P-cli"
BUILD_CTX="$(mktemp -d)"
PSK="client-profile-test"
FAILS=0

cleanup() {
    docker ps -a --format '{{.Names}}' | grep "^$P-" | xargs -r docker rm -f >/dev/null 2>&1 || true
    docker network rm "$NET_NAME" >/dev/null 2>&1 || true
    rm -rf "$BUILD_CTX"
}
[ -n "$KEEP" ] || trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

check() {   # check <condition-exit-code> <message>
    if [ "$1" = 0 ]; then echo "   ok: $2"; else echo "   FAIL: $2"; FAILS=$((FAILS + 1)); fi
}

[ -x "$CAMEX_BINARY" ] || { echo "camex binary not found: $CAMEX_BINARY"; exit 1; }
if [ -z "$CAMEX_CLIENT_BINARY" ]; then
    make -s -C "$REPO_DIR/src" PROFILE=client \
        BUILD_DIR="$BUILD_CTX/client" >/dev/null || { echo "cannot build the client profile"; exit 1; }
    CAMEX_CLIENT_BINARY="$BUILD_CTX/client/camex"
fi
CB=$CAMEX_CLIENT_BINARY
set +e   # checks below report failures themselves

echo "== 1. what the build has"
features=$("$CB" --version 2>&1 | sed -n 's/^features: //p')
echo "   features: $features"
[ "$features" = "client, l2, mesh" ]
check $? "--version: client, l2, mesh"
"$CAMEX_BINARY" --version 2>&1 | grep -q '^features:.* server'
check $? "the server under test has the server"

echo "== 2. what it refuses"
out=$("$CB" --mode server --port 5800 --psk x 2>&1); rc=$?
[ "$rc" != 0 ] && printf '%s\n' "$out" | grep -q "built without the server"
check $? "--mode server: refused, says why (exit $rc)"
for f in nginx-map nginx-index; do
    out=$("$CB" --status --format "$f" 2>&1); rc=$?
    [ "$rc" != 0 ] && printf '%s\n' "$out" | grep -q "built without the server"
    check $? "--status --format $f: refused, says why (exit $rc)"
done
! "$CB" --help 2>&1 | grep -qE '^Server options|--format nginx'
check $? "--help: no server options"

cp "$CAMEX_BINARY" "$BUILD_CTX/camex"
cp "$CB" "$BUILD_CTX/camex-client"
. "$SCRIPT_DIR/lib-keys.sh"
gen_server_key "$BUILD_CTX" "$CAMEX_BINARY" || { echo "cannot create a server key"; exit 1; }
cat > "$BUILD_CTX/Dockerfile" <<'DOCKERFILE'
FROM ubuntu:24.04
# New clients right after the server starts: no start hold (#81)
ENV CAMEX_POOL_HOLD=0
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
        iputils-ping iproute2 >/dev/null && rm -rf /var/lib/apt/lists/*
COPY camex camex-client /usr/local/bin/
COPY server.key /etc/camex/server.key
RUN printf '#!/bin/sh\nmkdir -p /dev/net\n[ -e /dev/net/tun ] || mknod /dev/net/tun c 10 200\nexec /usr/local/bin/${CAMEX:-camex} "$@"\n' \
        > /usr/local/bin/camex-run && chmod +x /usr/local/bin/camex-run
ENTRYPOINT ["/usr/local/bin/camex-run"]
DOCKERFILE
docker build -q -t "$IMAGE" "$BUILD_CTX" >/dev/null || { echo "cannot build $IMAGE"; exit 1; }
docker network create "$NET_NAME" >/dev/null || { echo "cannot create $NET_NAME"; exit 1; }

docker run -d --name "$SRV" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    "$IMAGE" --mode server --port 5800 \
    --local-cidr 10.200.0.1/24 $SERVER_KEY_ARGS --psk "$PSK" >/dev/null
sleep 1

for transport in udp tcp; do
    echo "== 3. $transport: the client profile against a full server"
    docker rm -f "$CLI" >/dev/null 2>&1
    docker run -d --name "$CLI" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
        -e CAMEX=camex-client \
        "$IMAGE" --mode client --auto --name "CPROF-$transport" --transport "$transport" \
        --server-host "$SRV" --port 5800 \
        --server-pubkey "$SERVER_PUBKEY" --psk "$PSK" >/dev/null
    up=1
    for _ in $(seq 1 20); do
        docker exec "$CLI" ping -c1 -W1 10.200.0.1 >/dev/null 2>&1 && { up=0; break; }
        sleep 1
    done
    check $up "$transport: the client reaches the server"
    CLI_IP=$(docker exec "$CLI" sh -c "ip -4 -o addr show 2>/dev/null | awk '/10\.200\./ { split(\$4, a, \"/\"); print a[1] }'" 2>/dev/null)
    [ -n "$CLI_IP" ] && docker exec "$SRV" ping -c 3 -W 1 "$CLI_IP" >/dev/null 2>&1
    check $? "$transport: the server reaches the client (${CLI_IP:-no address})"
done

if [ "$FAILS" != 0 ]; then
    echo "--- client log (tail) ---"
    docker logs "$CLI" 2>&1 | tail -10
    echo "--- server log (tail) ---"
    docker logs "$SRV" 2>&1 | tail -10
    echo "FAIL: $FAILS check(s) failed"
    exit 2
fi
echo "PASS: the client profile has no server and connects over UDP and TCP"
