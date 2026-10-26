#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-e2e-isolation.sh — client_isolation (#83)
#
# A packet from one client to another is delivered inside camex and never
# reaches the server's tunnel interface, so a firewall there cannot stop
# it. client_isolation=yes drops it.
#
# 1. client_isolation=no (the default): client A reaches client B.
# 2. A reload to yes: A no longer reaches B (nor B A); both still reach
#    the server and the server reaches both; camex --status counts the
#    dropped packets.
# 3. A reload back to no: A reaches B again.
# 4. --client-isolation on the command line.
#
# Usage: ./test-e2e-isolation.sh
#   CAMEX_BINARY=path/to/camex  binary to test (default: ../camex)
#   KEEP=1                      keep containers for debugging
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/camex}"
IMAGE="camex:e2e-isolation"
P="camex-iso-$$"
NET_NAME="$P-net"
BUILD_CTX="$(mktemp -d)"
PSK="isolation-test"
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
        python3-minimal iputils-ping iproute2 >/dev/null && rm -rf /var/lib/apt/lists/*
COPY camex /usr/local/bin/camex
COPY server.key /etc/camex/server.key
RUN printf '#!/bin/sh\nmkdir -p /dev/net\n[ -e /dev/net/tun ] || mknod /dev/net/tun c 10 200\nexec /usr/local/bin/camex "$@"\n' \
        > /usr/local/bin/camex-run && chmod +x /usr/local/bin/camex-run
ENTRYPOINT ["/usr/local/bin/camex-run"]
DOCKERFILE
docker build -q -t "$IMAGE" "$BUILD_CTX" >/dev/null 2>&1
docker network create "$NET_NAME" >/dev/null
set +e   # checks below report failures themselves

server() {  # server <config text> [camex args...]
    local conf="$1"; shift
    printf "$conf" > "$BUILD_CTX/srv.conf"
    docker create --name "$P-srv" --network "$NET_NAME" --cap-add NET_ADMIN \
        --cap-add MKNOD "$IMAGE" --mode server --port 5800 \
        --local-cidr 10.200.0.1/24 $SERVER_KEY_ARGS --psk "$PSK" \
        --config /etc/camex.conf "$@" >/dev/null
    docker cp "$BUILD_CTX/srv.conf" "$P-srv:/etc/camex.conf" >/dev/null
    docker start "$P-srv" >/dev/null
}

reload() {  # reload <config text>
    printf "$1" > "$BUILD_CTX/srv.conf"
    docker cp "$BUILD_CTX/srv.conf" "$P-srv:/etc/camex.conf" >/dev/null
    docker kill -s HUP "$P-srv" >/dev/null
    sleep 1
}

client() {  # client <name>
    docker run -d --name "$P-$1" --network "$NET_NAME" --cap-add NET_ADMIN \
        --cap-add MKNOD "$IMAGE" --mode client --auto --name "$1" \
        --server-host "$P-srv" --port 5800 --server-pubkey "$SERVER_PUBKEY" \
        --psk "$PSK" >/dev/null
}

addr_of() {  # addr_of <container>
    local a _
    for _ in $(seq 1 20); do
        a=$(docker exec "$P-$1" ip -4 -o addr show 2>/dev/null |
            awk '$2 != "lo" && $4 ~ /^10\.200\./ { sub("/.*", "", $4); print $4 }')
        [ -n "$a" ] && { echo "$a"; return 0; }
        sleep 1
    done
    return 1
}

reach() {  # reach <from container> <address>
    docker exec "$P-$1" ping -c 2 -W 2 "$2" >/dev/null 2>&1
}

server_side() {  # server_side: both clients reach the server and back
    reach A 10.200.0.1 && reach B 10.200.0.1 &&
        docker exec "$P-srv" ping -c 2 -W 2 "$a" >/dev/null 2>&1 &&
        docker exec "$P-srv" ping -c 2 -W 2 "$b" >/dev/null 2>&1
}

echo "== 1. client_isolation=no: clients reach each other"
server '[server]\nclient_isolation=no\n'
sleep 1
client A
client B
a=$(addr_of A); b=$(addr_of B)
echo "   A $a, B $b"
reach A "$b"
check $? "A reaches B"
server_side
check $? "both reach the server and the server reaches both"

echo "== 2. a reload to client_isolation=yes"
reload '[server]\nclient_isolation=yes\n'
docker logs "$P-srv" 2>&1 | grep -q "Client isolation: on"
check $? "the server logs the change"
! reach A "$b" && ! reach B "$a"
check $? "A does not reach B, nor B A"
server_side
check $? "both still reach the server and the server reaches both"
drops=$(docker exec "$P-srv" camex --status --json |
        python3 -c 'import json,sys; s=json.load(sys.stdin)["servers"][0]; print(s["client_isolation"], s["isolation_drops"], sum(c["isolation_drops"] for c in s["clients"]))')
set -- $drops
[ "$1" = True ] && [ "${2:-0}" -ge 2 ] && [ "$2" = "$3" ]
check $? "camex --status counts the dropped packets ($drops)"
docker exec "$P-srv" camex --status | grep -q "client isolation on"
check $? "the table says isolation is on"

echo "== 3. a reload back to no"
reload '[server]\n'
reach A "$b"
check $? "A reaches B again"

echo "== 4. --client-isolation on the command line"
docker rm -f "$P-srv" "$P-A" "$P-B" >/dev/null
server '[server]\n' --client-isolation
sleep 1
client A
client B
a=$(addr_of A); b=$(addr_of B)
! reach A "$b"
check $? "A does not reach B"
reload '[server]\nclient_isolation=no\n'
! reach A "$b"
check $? "the command line option outlasts a reload"

if [ "$FAILS" -ne 0 ]; then
    echo "FAIL: $FAILS check(s) failed"
    exit 1
fi
echo "PASS: client_isolation drops client-to-client packets only"
