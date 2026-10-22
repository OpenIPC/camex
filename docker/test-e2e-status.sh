#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-e2e-status.sh — camex --status lists a server's clients (#82)
#
# 1. A UDP server with two auto clients: the table and the JSON list both
#    with their IDs and addresses; traffic shows in the counters.
# 2. The socket is 0600: another user cannot ask.
# 3. A TCP server in the same container: --status lists both servers,
#    --json has both; --control asks one.
# 4. A client that is gone shows as a kept address (lease, #81).
# 5. control_socket=none: no socket.
# 6. A reload that changes control_socket= moves the socket.
#
# Usage: ./test-e2e-status.sh
#   CAMEX_BINARY=path/to/camex  binary to test (default: ../camex)
#   KEEP=1                      keep containers for debugging
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/camex}"
IMAGE="camex:e2e-status"
P="camex-status-$$"
NET_NAME="$P-net"
BUILD_CTX="$(mktemp -d)"
PSK="status-test"
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

client() {  # client <name> <server port> [camex args...]
    local name="$1" port="$2"; shift 2
    docker run -d --name "$P-$name" --network "$NET_NAME" --cap-add NET_ADMIN \
        --cap-add MKNOD "$IMAGE" --mode client --auto --name "$name" \
        --server-host "$P-srv" --port "$port" --server-pubkey "$SERVER_PUBKEY" \
        --psk "$PSK" "$@" >/dev/null
}

status() {  # status [args...]: camex --status in the server container
    docker exec "$P-srv" camex --status "$@"
}

json_get() {  # json_get <python expression on d>: from --status --json
    status --json | python3 -c "import json,sys; d=json.load(sys.stdin); print($1)" 2>/dev/null
}

echo "== 1. a UDP server with two clients"
docker run -d --name "$P-srv" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    -e CAMEX_CLIENT_TIMEOUT=5 "$IMAGE" --mode server --port 5800 \
    --local-cidr 10.200.0.1/24 $SERVER_KEY_ARGS --psk "$PSK" >/dev/null
sleep 1
client CAM-A 5800
client CAM-B 5800
sleep 4
status > "$BUILD_CTX/s1.txt"
check $? "camex --status answers"
cat "$BUILD_CTX/s1.txt" | sed 's/^/      /'
grep -q "udp port 5800, tunnel 10.200.0.1/24, pool 10.200.0.0/24" "$BUILD_CTX/s1.txt" &&
    grep -q "clients 2 of" "$BUILD_CTX/s1.txt"
check $? "the header names the server and the client count"
grep -q " HS " "$BUILD_CTX/s1.txt" && grep -q "RX PKTS" "$BUILD_CTX/s1.txt" &&
    grep -Eq "^CAM-A +auto +global +10\.200\.0\.[0-9]+ " "$BUILD_CTX/s1.txt" &&
    grep -Eq "^CAM-B +auto +global +10\.200\.0\.[0-9]+ " "$BUILD_CTX/s1.txt"
check $? "the table lists both clients with their addresses"
a_ip=$(json_get "[c['address'] for c in d['servers'][0]['clients'] if c['id']=='CAM-A'][0]")
[ -n "$a_ip" ] && [ "$a_ip" = "$(docker exec "$P-CAM-A" ip -4 -o addr show | awk '$4 ~ /^10\.200\./ {sub("/.*","",$4); print $4}')" ]
check $? "the JSON address of CAM-A is the one it has ($a_ip)"
[ "$(json_get "len(d['servers'][0]['clients'])")" = 2 ] &&
    [ "$(json_get "d['servers'][0]['transport']+' '+str(d['servers'][0]['port'])")" = "udp 5800" ]
check $? "the JSON has the server and two clients"
before=$(json_get "[c['tx_packets'] for c in d['servers'][0]['clients'] if c['id']=='CAM-A'][0]")
docker exec "$P-srv" ping -c 5 -W 2 "$a_ip" >/dev/null 2>&1
after=$(json_get "[c['tx_packets'] for c in d['servers'][0]['clients'] if c['id']=='CAM-A'][0]")
rx=$(json_get "[c['rx_packets'] for c in d['servers'][0]['clients'] if c['id']=='CAM-A'][0]")
[ -n "$after" ] && [ "$after" -ge $((before + 5)) ] && [ "$rx" -ge 5 ]
check $? "the counters follow the traffic (tx $before -> $after, rx $rx)"

echo "== 2. only the server's user can ask"
docker exec "$P-srv" sh -c 'ls -l /run/camex/camex-udp-5800.sock' | grep -q '^srw-------'
check $? "the socket is 0600"
docker exec -u 65534 "$P-srv" camex --status >/dev/null 2>"$BUILD_CTX/denied.txt"
[ $? != 0 ] && grep -q "Permission denied" "$BUILD_CTX/denied.txt"
check $? "another user is refused ($(head -c 120 "$BUILD_CTX/denied.txt"))"

echo "== 3. a TCP server on the same host"
docker exec -d "$P-srv" camex --mode server --transport tcp --port 5801 \
    --local-cidr 10.201.0.1/24 $SERVER_KEY_ARGS --psk "$PSK"
sleep 1
docker run -d --name "$P-CAM-T" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    "$IMAGE" --mode client --auto --name CAM-T --server-host "$P-srv" --port 5801 \
    --transport tcp --server-pubkey "$SERVER_PUBKEY" --psk "$PSK" >/dev/null
sleep 4
status > "$BUILD_CTX/s3.txt"
grep -q "^== /run/camex/camex-tcp-5801.sock" "$BUILD_CTX/s3.txt" &&
    grep -q "^== /run/camex/camex-udp-5800.sock" "$BUILD_CTX/s3.txt" &&
    grep -Eq "^CAM-T +auto" "$BUILD_CTX/s3.txt"
check $? "--status lists both servers"
[ "$(json_get "' '.join(s['transport'] for s in d['servers'])")" = "tcp udp" ]
check $? "--json has both servers"
status --control /run/camex/camex-tcp-5801.sock > "$BUILD_CTX/s3t.txt"
grep -Eq "^CAM-T " "$BUILD_CTX/s3t.txt" && ! grep -q "CAM-A" "$BUILD_CTX/s3t.txt"
check $? "--control asks only that server"

echo "== 4. a client that is gone keeps its address"
docker rm -f "$P-CAM-B" >/dev/null
gone=1
for _ in $(seq 1 20); do
    [ "$(json_get "[l['id'] for l in d['servers'][1]['leases']]")" = "['CAM-B']" ] && { gone=0; break; }
    sleep 1
done
check $gone "CAM-B is listed as a kept address"
status --control /run/camex/camex-udp-5800.sock | grep -Eq "^CAM-B +10\.200\.0\.[0-9]+ +gone"
check $? "the table shows it under the kept addresses"

echo "== 5. control_socket=none"
printf '[server]\ncontrol_socket=none\n' > "$BUILD_CTX/none.conf"
docker cp "$BUILD_CTX/none.conf" "$P-srv:/etc/none.conf" >/dev/null
docker exec -d "$P-srv" camex --mode server --port 5802 --local-cidr 10.202.0.1/24 \
    $SERVER_KEY_ARGS --psk "$PSK" --config /etc/none.conf
sleep 1
! docker exec "$P-srv" test -e /run/camex/camex-udp-5802.sock
check $? "no socket for that server"

echo "== 6. a reload moves the socket"
printf '[server]\n' > "$BUILD_CTX/mv.conf"
docker cp "$BUILD_CTX/mv.conf" "$P-srv:/etc/mv.conf" >/dev/null
docker exec -d "$P-srv" camex --mode server --port 5803 --local-cidr 10.203.0.1/24 \
    $SERVER_KEY_ARGS --psk "$PSK" --config /etc/mv.conf --pid-file /run/mv.pid
sleep 1
docker exec "$P-srv" test -S /run/camex/camex-udp-5803.sock
check $? "the server listens on its default socket"
printf '[server]\ncontrol_socket=/run/camex/moved.sock\n' > "$BUILD_CTX/mv.conf"
docker cp "$BUILD_CTX/mv.conf" "$P-srv:/etc/mv.conf" >/dev/null
docker exec "$P-srv" sh -c 'kill -HUP $(cat /run/mv.pid)'
sleep 2
docker exec "$P-srv" test -S /run/camex/moved.sock &&
    ! docker exec "$P-srv" test -e /run/camex/camex-udp-5803.sock &&
    status --control /run/camex/moved.sock | grep -q "udp port 5803"
check $? "after the reload it answers on the new path only"

if [ "$FAILS" -ne 0 ]; then
    echo "FAIL: $FAILS check(s) failed"
    exit 1
fi
echo "PASS: camex --status lists the servers and their clients"
