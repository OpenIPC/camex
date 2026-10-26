#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-e2e-client-start.sh — a client started where the network is not
# ready waits instead of exiting (#121)
#
# 1. --auto without --name and no default route: a bad server key is
#    reported at once; otherwise the client waits, and once
#    a default route is there, its ID is the MAC of that interface, and
#    once the server answered it is kept in /tmp/camex-id.
# 2. A restart without a default route takes the ID from /tmp/camex-id.
# 3. A /tmp/camex-id others can write is ignored, with a warning.
# 4. No answer from the server (a wrong route), then the client's address
#    changes and the route is fixed: the client gets through, it does not
#    exit (it used to: the old source address, or 300 s of waiting).
# 5. --keepalive 0: a server that stops answering is noticed (60 s +
#    --server-timeout), and the client connects again when it is back.
#
# Usage: ./test-e2e-client-start.sh
#   CAMEX_BINARY=path/to/camex  binary to test (default: ../camex)
#   KEEP=1                      keep containers for debugging
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/camex}"
IMAGE="camex:e2e-client-start"
P="camex-start-$$"
NET_NAME="$P-net"
BUILD_CTX="$(mktemp -d)"
PSK="start-test"
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
sleep 1
SRV=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$P-srv")

box() {     # box <name>: a container to run clients in by hand
    docker run -d --name "$P-$1" --network "$NET_NAME" --cap-add NET_ADMIN \
        --cap-add MKNOD --entrypoint sleep "$IMAGE" infinity >/dev/null
    docker exec "$P-$1" sh -c 'mkdir -p /dev/net && mknod /dev/net/tun c 10 200'
}

start() {   # start <box> [camex args...]: a client in the background, its
            # log in /tmp/log (started anew)
    local b="$1"; shift
    docker exec "$P-$b" sh -c 'pkill -x camex; sleep 1; : > /tmp/log'
    docker exec -d "$P-$b" sh -c "camex --mode client --auto --server-host $SRV \
        --port 5800 --server-pubkey $SERVER_PUBKEY --psk $PSK $* >> /tmp/log 2>&1;
        echo EXITED >> /tmp/log"
}

log_of() {  # log_of <box>
    docker exec "$P-$1" cat /tmp/log
}

wait_log() {  # wait_log <box> <pattern> <seconds>
    local _
    for _ in $(seq 1 "$3"); do
        log_of "$1" | grep -q "$2" && return 0
        sleep 1
    done
    return 1
}

registered() {  # registered <id> <seconds>: the server registered it
    local _
    for _ in $(seq 1 "$2"); do
        docker logs "$P-srv" 2>&1 | grep -q "Registered auto client $1\$" && return 0
        sleep 1
    done
    return 1
}

echo "== 1. no default route at the start"
box a
MAC=$(docker exec "$P-a" cat /sys/class/net/eth0/address | tr -d ':' | tr 'a-f' 'A-F')
GW=$(docker exec "$P-a" sh -c "ip route | awk '/^default/ {print \$3}'")
docker exec "$P-a" ip route del default
# A bad server key is reported at once, not after a default route came
start a --keepalive 2 --server-pubkey AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=
wait_log a EXITED 5 && ! log_of a | grep -q "No default route"
check $? "a bad server key: it stops at once, without waiting for a route"
start a --keepalive 2
sleep 8
log_of a | grep -q "No default route (or no MAC on its interface) yet" &&
    ! log_of a | grep -q EXITED
check $? "it waits for a default route instead of exiting"
docker exec "$P-a" ip route add default via "$GW"
wait_log a "Client ID $MAC: the MAC of eth0" 10 && registered "$MAC" 20
check $? "with a default route: the ID is eth0's MAC ($MAC), the server registers it"
wait_log a "kept in /tmp/camex-id" 5 &&
    [ "$(docker exec "$P-a" cat /tmp/camex-id)" = "$MAC" ] &&
    [ "$(docker exec "$P-a" stat -c %a /tmp/camex-id)" = 644 ]
check $? "the ID is kept in /tmp/camex-id"

echo "== 2. a restart takes the kept ID"
docker exec "$P-a" ip route del default
start a --keepalive 2
wait_log a "Client ID $MAC, kept in /tmp/camex-id by an earlier run" 10 &&
    ! log_of a | grep -q "No default route"
check $? "no default route needed: the ID comes from /tmp/camex-id"
wait_log a "established" 20
check $? "and it connects"

echo "== 3. a file others can write"
docker exec "$P-a" sh -c 'echo AAAAAAAAAAAA > /tmp/camex-id && chmod 666 /tmp/camex-id'
docker exec "$P-a" ip route add default via "$GW"
start a --keepalive 2
wait_log a "is not a file of this user that only it can write: ignored" 10 &&
    wait_log a "Client ID $MAC: the MAC of eth0" 10
check $? "it is ignored with a warning, the ID is the MAC"
docker exec "$P-a" sh -c 'printf "AAAAAAAAAAAA\nmore\n" > /tmp/camex-id && chmod 644 /tmp/camex-id'
start a --keepalive 2
wait_log a "holds no client ID: ignored" 10 &&
    wait_log a "Client ID $MAC: the MAC of eth0" 10
check $? "an ID with more after it is ignored too"

echo "== 4. no answer, then a new address and the right route"
box b
docker exec "$P-b" ip route add "$SRV/32" via "${SRV%.*}.250"
start b --name CAM-B --keepalive 2
sleep 25
old=$(docker exec "$P-b" sh -c "ip -4 -o addr show eth0 | awk '{print \$4}'")
# The old address goes first, as a reconfiguration does (deleting a
# primary address also deletes the others where promote_secondaries is off)
docker exec "$P-b" sh -c "ip addr del $old dev eth0 && ip addr add ${SRV%.*}.177/${old#*/} dev eth0 &&
    ip route add default via $GW; ip -4 -o addr show eth0; ip route"
registered CAM-B 40 && ! log_of b | grep -q EXITED
check $? "it gets through from the new address ($old -> .177) without exiting"
log_of b | grep -q "No answer from server .*: a new connection"
check $? "it said it started over with a new connection"

echo "== 5. --keepalive 0 notices a server that is gone"
box c
start c --name CAM-C --keepalive 0
wait_log c "established" 20
check $? "the client is connected"
docker pause "$P-srv" >/dev/null
t0=$(date +%s)
wait_log c "Connection lost" 100
rc=$?
t1=$(date +%s)
check $rc "the lost server is noticed ($((t1 - t0)) s, at most 60 + 20)"
# Counted before the server is back: the client may connect at once
n=$(log_of c | grep -c "established")
docker unpause "$P-srv" >/dev/null
for _ in $(seq 1 40); do
    [ "$(log_of c | grep -c established)" -gt "$n" ] && break
    sleep 1
done
[ "$(log_of c | grep -c established)" -gt "$n" ]
check $? "and it connects again when the server is back"

if [ "$FAILS" -ne 0 ]; then
    echo "--- client a"; log_of a | tail -20
    echo "--- client b"; log_of b | tail -20
    echo "--- client c"; log_of c | tail -20
    echo "FAIL: $FAILS check(s) failed"
    exit 1
fi
echo "PASS: the client waits for the network at its start"
