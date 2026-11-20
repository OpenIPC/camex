#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-e2e-tap.sh — TAP networks (mode = tap, the full camex: CAMEX_L2=1)
#
#  1. Two clients join a TAP network: each gets a TAP interface with the
#     MAC the server derives (locally administered) and its address
#  2. They reach the gateway (the server answers ARP for it) and each
#     other (the server switches their frames)
#  3. MTU 1500: a 1500-byte IP packet (a 1514-byte frame) with DF set goes
#     both ways, in camex's own pieces, never as IP fragments
#  4. The server sends a frame only to the client its MAC is behind
#  5. A core camex (without CAMEX_L2) refuses a TAP network: the client
#     says why, and a server does not start with mode = tap
#
# Usage: ./test-e2e-tap.sh
#   CAMEX_BINARY=path/to/camex            the core camex (default: ../camex)
#   CAMEX_FULL_BINARY=path/to/camex-full  the full one (default: built here)
#   KEEP=1                                keep containers for debugging
#

set -e
export LC_ALL=C

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/camex}"
IMAGE="camex:e2e-tap"
P="camex-tap-$$"
NET_NAME="$P-net"
BUILD_CTX="$(mktemp -d)"
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
if [ -z "$CAMEX_FULL_BINARY" ]; then
    make -s -C "$REPO_DIR/src" CAMEX_L2=1 CAMEX_MESH=1 \
        BUILD_DIR="$BUILD_CTX/full" >/dev/null || { echo "cannot build the full camex"; exit 1; }
    CAMEX_FULL_BINARY="$BUILD_CTX/full/camex"
fi

cp "$CAMEX_BINARY" "$BUILD_CTX/camex-core"
cp "$CAMEX_FULL_BINARY" "$BUILD_CTX/camex"
. "$SCRIPT_DIR/lib-keys.sh"
gen_server_key "$BUILD_CTX" "$CAMEX_BINARY" || { echo "cannot create a server key"; exit 1; }
cat > "$BUILD_CTX/Dockerfile" <<'DOCKERFILE'
FROM ubuntu:24.04
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
        iputils-ping iproute2 tcpdump >/dev/null && rm -rf /var/lib/apt/lists/*
COPY camex camex-core /usr/local/bin/
COPY server.key /etc/camex/server.key
RUN printf '#!/bin/sh\nmkdir -p /dev/net\n[ -e /dev/net/tun ] || mknod /dev/net/tun c 10 200\nexec /usr/local/bin/${CAMEX:-camex} "$@"\n' \
        > /usr/local/bin/camex-run && chmod +x /usr/local/bin/camex-run
ENTRYPOINT ["/usr/local/bin/camex-run"]
DOCKERFILE
docker build -q -t "$IMAGE" "$BUILD_CTX" >/dev/null 2>&1
docker network create "$NET_NAME" >/dev/null
set +e

cat > "$BUILD_CTX/camex.conf" <<'EOF'
[server]
local_cidr = 10.200.0.1/24

[network lan]
weak_name = yes
mode = tap
pool = 10.66.0.0/24
EOF

docker create --name "$P-srv" --network "$NET_NAME" --cap-add NET_ADMIN \
    --cap-add MKNOD "$IMAGE" --mode server --port 5800 $SERVER_KEY_ARGS \
    --config /etc/camex.conf >/dev/null
docker cp "$BUILD_CTX/camex.conf" "$P-srv:/etc/camex.conf" >/dev/null
docker start "$P-srv" >/dev/null
sleep 1

client() {  # client <name> [docker args...] -- [camex args...]
    local name="$1"; shift
    local dargs=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do dargs+=("$1"); shift; done
    shift
    docker run -d --name "$P-$name" --network "$NET_NAME" --cap-add NET_ADMIN \
        --cap-add MKNOD "${dargs[@]}" "$IMAGE" --server-host "$P-srv" \
        --network lan "$@" >/dev/null
}

tap_of() {  # tap_of <container>: "<ifname> <mac> <address/prefix>"
    local i l
    for i in $(seq 1 30); do
        l=$(docker exec "$1" sh -c 'for d in /sys/class/net/tap*; do
                [ -e "$d" ] || continue; n=${d##*/}
                a=$(ip -4 -o addr show dev $n | awk "{ print \$4 }")
                [ -n "$a" ] && echo "$n $(cat $d/address) $a"; done' 2>/dev/null)
        [ -n "$l" ] && { echo "$l"; return 0; }
        sleep 0.5
    done
    return 1
}

echo "== 1. two clients of a TAP network"
client a -- --name TAP-A
client b -- --name TAP-B
read -r IFA MACA A <<<"$(tap_of "$P-a")"
read -r IFB MACB B <<<"$(tap_of "$P-b")"
case "$A" in 10.66.0.*/24) r=0 ;; *) r=1 ;; esac
check $r "A has a TAP interface $IFA, $MACA, $A"
case "$B" in 10.66.0.*/24) r=0 ;; *) r=1 ;; esac
check $r "B has a TAP interface $IFB, $MACB, $B"
case "$MACA" in ?[26ae]:*) r=0 ;; *) r=1 ;; esac
check $r "the MAC is locally administered ($MACA)"
docker restart "$P-a" >/dev/null
read -r IFA2 MACA2 A2 <<<"$(tap_of "$P-a")"
[ "$MACA2" = "$MACA" ] && [ "$A2" = "$A" ]
check $? "after a restart: the same MAC and address ($MACA2 $A2)"

echo "== 2. the gateway and each other"
docker exec "$P-a" ping -c 2 -W 2 10.66.0.1 >/dev/null
check $? "A reaches the gateway 10.66.0.1"
docker exec "$P-a" ping -c 2 -W 2 "${B%/*}" >/dev/null
check $? "A reaches B"
docker exec "$P-b" ip neigh show "${A%/*}" | grep -q "$MACA"
check $? "B learned A's MAC by ARP through the server"

echo "== 3. MTU 1500: 1514-byte frames, DF set"
docker exec "$P-a" ping -c 3 -W 2 -M do -s 1472 "${B%/*}" >/dev/null
check $? "1500-byte packets A -> B -> A"
docker exec "$P-b" ping -c 3 -W 2 -M do -s 1472 10.66.0.1 >/dev/null
check $? "1500-byte packets to the gateway and back"
# On the wire between the hosts: no IP fragments, every datagram fits
docker exec -d "$P-a" sh -c "tcpdump -n -i eth0 -c 40 'udp port 5800' > /tmp/wire.txt 2>/dev/null"
sleep 1
docker exec "$P-a" ping -c 5 -i 0.2 -W 2 -M do -s 1472 "${B%/*}" >/dev/null
sleep 1
docker exec "$P-a" sh -c 'pkill tcpdump; sleep 0.5; cat /tmp/wire.txt' > "$BUILD_CTX/wire.txt"
grep -q "length" "$BUILD_CTX/wire.txt"
check $? "tcpdump saw the tunnel"
! grep -q "frag" "$BUILD_CTX/wire.txt"
check $? "no IP fragments on the wire"
max=$(sed -n 's/.*UDP, length \([0-9]*\).*/\1/p' "$BUILD_CTX/wire.txt" | sort -n | tail -1)
[ -n "$max" ] && [ "$max" -le 1400 ]
check $? "the largest datagram is $max bytes (at most 1400)"

echo "== 4. unicast only to the client behind the MAC"
client c -- --name TAP-C
read -r IFC MACC C <<<"$(tap_of "$P-c")"
docker exec "$P-c" ping -c 1 -W 2 10.66.0.1 >/dev/null   # C's MAC learned
docker exec -d "$P-c" sh -c "tcpdump -n -i $IFC -c 100 icmp > /tmp/c.txt 2>/dev/null"
sleep 1
docker exec "$P-a" ping -c 3 -W 2 "${B%/*}" >/dev/null
sleep 1
docker exec "$P-c" sh -c 'pkill tcpdump; sleep 0.5; cat /tmp/c.txt' > "$BUILD_CTX/c.txt"
! grep -q "${B%/*}" "$BUILD_CTX/c.txt"
check $? "C does not see A's pings to B"

echo "== 5. a core camex and a TAP network"
client core -e CAMEX=camex-core -- --name CORE-D
sleep 4
docker logs "$P-core" 2>&1 | grep -q "built without CAMEX_L2"
check $? "the core client says it cannot join a TAP network"
docker create --name "$P-coresrv" --network "$NET_NAME" --cap-add NET_ADMIN \
    --cap-add MKNOD -e CAMEX=camex-core "$IMAGE" --mode server --port 5800 \
    $SERVER_KEY_ARGS --config /etc/camex.conf >/dev/null
docker cp "$BUILD_CTX/camex.conf" "$P-coresrv:/etc/camex.conf" >/dev/null
docker start "$P-coresrv" >/dev/null
sleep 2
docker logs "$P-coresrv" 2>&1 | grep -q "mode = tap needs a camex built with CAMEX_L2=1"
check $? "a core server refuses mode = tap"

echo
if [ "$FAILS" = 0 ]; then
    echo "test-e2e-tap: all checks passed"
else
    echo "test-e2e-tap: $FAILS check(s) FAILED"
    exit 1
fi
