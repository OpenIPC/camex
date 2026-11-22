#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-e2e-both.sh — a server on UDP and TCP at once, one pool (#178)
#
# 1. A server without --transport listens on UDP and TCP on its port: the
#    status header says udp+tcp, the JSON "both", the socket is
#    camex-both-<port>.sock.
# 2. A client over UDP and one over TCP at once: both get an address from
#    the one pool, reach the server and each other; the status and the
#    control socket's records show each one's transport.
# 3. A client that comes back over the other transport with the same ID
#    keeps its address: UDP -> TCP -> UDP after a restart, and a TCP client
#    cut off without its connection closing that comes back over UDP (the
#    server moves the live entry and closes the old connection).
# 4. Two clients with one ID over UDP and TCP at once: one entry, the
#    server keeps serving.
# 5. A server in both whose TCP or UDP port is taken (also by a UDP server
#    that binds with SO_REUSEADDR) does not start and says why;
#    --transport both is refused for a client.
# 6. The TCP limit (select(), open files) of a server on both counts only
#    TCP clients, keeps room for pre-auth connections, and leaves UDP
#    clients alone.
#
# Usage: ./test-e2e-both.sh
#   CAMEX_BINARY=path/to/camex  binary to test (default: ../camex)
#   KEEP=1                      keep containers for debugging
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/camex}"
IMAGE="camex:e2e-both"
P="camex-both-$$"
NET_NAME="$P-net"
BUILD_CTX="$(mktemp -d)"
PSK="both-test"
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

client() {  # client <container> <ID> <udp|tcp> [camex args...]: started
    client_as "run -d" "$@"
}

client_created() {  # client_created <container> <ID> <udp|tcp>: created, not started
    client_as create "$@"
}

client_as() {  # client_as <"run -d"|create> <container> <ID> <udp|tcp> [camex args...]
    local verb="$1" name="$2" id="$3" tr="$4"; shift 4
    # shellcheck disable=SC2086  # the verb is one or two words
    docker $verb --name "$P-$name" --network "$NET_NAME" --cap-add NET_ADMIN \
        --cap-add MKNOD "$IMAGE" --mode client --auto --name "$id" \
        --server-host "$P-srv" --port 5800 --transport "$tr" \
        --server-pubkey "$SERVER_PUBKEY" --psk "$PSK" --keepalive 2 "$@" >/dev/null
}

status() {  # status [args...]: camex --status in the server container
    docker exec "$P-srv" camex --status "$@"
}

json_get() {  # json_get <python expression on d>: from --status --json
    status --json | python3 -c "import json,sys; d=json.load(sys.stdin); print($1)" 2>/dev/null
}

client_of() {  # client_of <ID> <field>: a field of a client in the JSON
    json_get "[c['$2'] for c in d['servers'][0]['clients'] if c['id']=='$1'][0]"
}

tun_addr() {  # tun_addr <container>: its tunnel address
    docker exec "$P-$1" ip -4 -o addr show 2>/dev/null |
        awk '$4 ~ /^10\.200\./ {sub("/.*","",$4); print $4}'
}

wait_for() {  # wait_for <seconds> <command...>: until it succeeds
    local n="$1"; shift
    for _ in $(seq 1 "$n"); do "$@" && return 0; sleep 1; done
    return 1
}

has_transport() {  # has_transport <ID> <udp|tcp>
    [ "$(client_of "$1" transport)" = "$2" ]
}

echo "== 1. a server without --transport listens on UDP and TCP"
docker run -d --name "$P-srv" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    -e CAMEX_CLIENT_TIMEOUT=6 "$IMAGE" --mode server --port 5800 \
    --local-cidr 10.200.0.1/24 $SERVER_KEY_ARGS --psk "$PSK" >/dev/null
sleep 1
docker exec "$P-srv" ss -uln | grep -q ':5800 ' && docker exec "$P-srv" ss -tln | grep -q ':5800 '
check $? "UDP and TCP port 5800 are open"
status | grep -q "udp+tcp port 5800, tunnel 10.200.0.1/24"
check $? "the status header says udp+tcp"
[ "$(json_get "d['servers'][0]['transport']")" = both ]
check $? "the JSON says both"
docker exec "$P-srv" test -S /run/camex/camex-both-5800.sock
check $? "the socket is camex-both-5800.sock"
docker logs "$P-srv" 2>&1 | grep -q "(UDP and TCP)"
check $? "the log names both transports"

echo "== 2. a UDP and a TCP client at once"
client U CAM-U udp
client T CAM-T tcp
wait_for 15 has_transport CAM-U udp && wait_for 15 has_transport CAM-T tcp
check $? "the status shows CAM-U over udp and CAM-T over tcp"
u_ip=$(tun_addr U); t_ip=$(tun_addr T)
[ -n "$u_ip" ] && [ -n "$t_ip" ] && [ "$u_ip" != "$t_ip" ]
check $? "both have an address from the one pool ($u_ip, $t_ip)"
docker logs "$P-U" 2>&1 | grep -q "Tunnel up: .*, MTU 1420, .*(UDP)" &&
    docker logs "$P-T" 2>&1 | grep -q "Tunnel up: .*, MTU 1500, .*(TCP)" &&
    docker logs "$P-srv" 2>&1 | grep -q "Tunnel up: .*, MTU 1420, listening"
check $? "no MTU given: 1420 over UDP and on the server of both, 1500 over TCP (#198)"
docker exec "$P-U" ping -c 2 -W 2 10.200.0.1 >/dev/null 2>&1 &&
    docker exec "$P-T" ping -c 2 -W 2 10.200.0.1 >/dev/null 2>&1
check $? "both reach the server"
docker exec "$P-U" ping -c 2 -W 2 "$t_ip" >/dev/null 2>&1 &&
    docker exec "$P-T" ping -c 2 -W 2 "$u_ip" >/dev/null 2>&1
check $? "they reach each other (UDP to TCP and back)"
status | grep -Eq "^CAM-U .* udp " && status | grep -Eq "^CAM-T .* tcp "
check $? "the table shows each one's transport"
ctl() {  # ctl <command>: the server's raw answer on its control socket
    docker exec "$P-srv" python3 -c "import socket,sys
s=socket.socket(socket.AF_UNIX); s.connect('/run/camex/camex-both-5800.sock')
s.sendall(sys.argv[1].encode()+b'\\n'); d=b''
while not d.endswith(b'\\n.\\n'):
    c=s.recv(65536)
    if not c: break
    d+=c
print(d.decode(), end='')" "$1"
}
ctl "CLIENTS INDEX" | grep -q "^L	CAM-T	" && ! ctl "CLIENTS INDEX" | grep -q "^L:" &&
    ctl "CLIENTS INDEX VIA" | grep -q "^L:tcp	CAM-T	" && ctl "CLIENTS INDEX VIA" | grep -q "^L:udp	CAM-U	"
check $? "CLIENTS INDEX keeps its old records for older readers, CLIENTS INDEX VIA has the transport"
# (the start page's Proto column from those records: tests/test_nginxmap.c)

echo "== 3. a client that changes transport keeps its address"
client S1 CAM-S udp
wait_for 15 has_transport CAM-S udp
s_ip=$(tun_addr S1)
[ -n "$s_ip" ]
check $? "CAM-S over UDP has $s_ip"
docker rm -f "$P-S1" >/dev/null
client S2 CAM-S tcp
wait_for 20 has_transport CAM-S tcp
check $? "CAM-S comes back over TCP"
wait_for 10 sh -c "[ \"\$(docker exec $P-S2 ip -4 -o addr show 2>/dev/null | grep -c ' $s_ip/')\" = 1 ]"
check $? "with the same address $s_ip"
docker exec "$P-S2" ping -c 2 -W 2 10.200.0.1 >/dev/null 2>&1
check $? "and reaches the server over TCP"
# Cut off: the TCP connection is not closed, the server's entry stays live.
# It stays live only for the client timeout (6 s here), so S3 is created
# beforehand and only started after the cut: a slow docker run would let
# the entry expire, and S3 would then register anew, not move it (#195).
client_created S3 CAM-S udp
docker network disconnect "$NET_NAME" "$P-S2" >/dev/null 2>&1
docker start "$P-S3" >/dev/null
wait_for 20 has_transport CAM-S udp
check $? "the live TCP entry moves to UDP when CAM-S comes back over UDP"
wait_for 10 sh -c "[ \"\$(docker exec $P-S3 ip -4 -o addr show 2>/dev/null | grep -c ' $s_ip/')\" = 1 ]"
check $? "again with $s_ip"
docker exec "$P-S3" ping -c 3 -W 2 10.200.0.1 >/dev/null 2>&1
check $? "and reaches the server over UDP"
wait_for 10 sh -c "docker logs $P-srv 2>&1 | grep -q 'Client CAM-S moved from TCP'"
check $? "the server logs the move and closes the old connection"
[ "$(json_get "len([c for c in d['servers'][0]['clients'] if c['id']=='CAM-S'])")" = 1 ]
check $? "one entry for CAM-S"
cs=$(client_of CAM-S connected_s)
[ -n "$cs" ] && [ "$cs" -lt 30 ]
check $? "its connection age starts again with the move (${cs:-?} s)"
docker rm -f "$P-S2" >/dev/null

echo "== 4. one ID over UDP and TCP at once"
client D1 CAM-D udp
client D2 CAM-D tcp
sleep 8
[ "$(json_get "len([c for c in d['servers'][0]['clients'] if c['id']=='CAM-D'])")" = 1 ]
check $? "one entry for CAM-D"
docker exec "$P-U" ping -c 2 -W 2 10.200.0.1 >/dev/null 2>&1 &&
    docker exec "$P-T" ping -c 2 -W 2 10.200.0.1 >/dev/null 2>&1 &&
    status >/dev/null
check $? "the server keeps serving the others"
docker rm -f "$P-D1" "$P-D2" >/dev/null

echo "== 5. a taken port, a client with both"
docker exec -d "$P-srv" camex --mode server --transport tcp --port 5900 \
    --local-cidr 10.210.0.1/24 $SERVER_KEY_ARGS --psk "$PSK"
sleep 1
docker exec "$P-srv" timeout 10 camex --mode server --port 5900 \
    --local-cidr 10.211.0.1/24 $SERVER_KEY_ARGS --psk "$PSK" > "$BUILD_CTX/taken.txt" 2>&1
rc=$?
[ "$rc" != 0 ] && [ "$rc" != 124 ] &&
    grep -q "TCP port 5900 is not available" "$BUILD_CTX/taken.txt" &&
    grep -q -- "--transport udp" "$BUILD_CTX/taken.txt"
check $? "a server in both does not start on a taken TCP port (rc $rc): $(grep -m1 'TCP port' "$BUILD_CTX/taken.txt")"
! docker exec "$P-srv" test -e /run/camex/camex-both-5900.sock
check $? "and leaves no socket behind"
# A UDP server holds the port (it binds with SO_REUSEADDR): a server in
# both must not share it
docker exec -d "$P-srv" camex --mode server --transport udp --port 5901 \
    --local-cidr 10.212.0.1/24 $SERVER_KEY_ARGS --psk "$PSK"
sleep 1
docker exec "$P-srv" timeout 10 camex --mode server --port 5901 \
    --local-cidr 10.213.0.1/24 $SERVER_KEY_ARGS --psk "$PSK" > "$BUILD_CTX/utaken.txt" 2>&1
rc=$?
[ "$rc" != 0 ] && [ "$rc" != 124 ] &&
    grep -q "UDP port 5901 is not available" "$BUILD_CTX/utaken.txt" &&
    grep -q -- "--transport tcp" "$BUILD_CTX/utaken.txt"
check $? "a server in both does not start on a UDP port another server holds (rc $rc): $(grep -m1 'UDP port' "$BUILD_CTX/utaken.txt")"
docker run --rm --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD "$IMAGE" \
    --mode client --auto --name CAM-X --server-host "$P-srv" --port 5800 \
    --transport both --server-pubkey "$SERVER_PUBKEY" --psk "$PSK" > "$BUILD_CTX/cboth.txt" 2>&1
[ $? != 0 ] && grep -q "both is for a server" "$BUILD_CTX/cboth.txt"
check $? "--transport both is refused for a client"

docker exec "$P-srv" true
check $? "the server is still running"

echo "== 6. the TCP limit of a server on both"
# nofile 40: 8 TCP connections (40 - 32), 2 of them kept for pre-auth ones
docker run -d --name "$P-lim" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    --ulimit nofile=40:40 "$IMAGE" --mode server --port 5800 \
    --local-cidr 10.220.0.1/24 $SERVER_KEY_ARGS --psk "$PSK" >/dev/null
sleep 1
lim_client() {  # lim_client <n> <udp|tcp>
    docker run -d --name "$P-L$1" --network "$NET_NAME" --cap-add NET_ADMIN \
        --cap-add MKNOD "$IMAGE" --mode client --auto --name "LIM$1" \
        --server-host "$P-lim" --port 5800 --transport "$2" \
        --server-pubkey "$SERVER_PUBKEY" --psk "$PSK" --keepalive 2 >/dev/null
}
for n in 1 2 3 4 5 6 7; do lim_client $n tcp; done
lim_client 8 udp; lim_client 9 udp
sleep 10
lim_json() { docker exec "$P-lim" camex --status --json | python3 -c "import json,sys; d=json.load(sys.stdin); print($1)" 2>/dev/null; }
tcp_n=$(lim_json "len([c for c in d['servers'][0]['clients'] if c['transport']=='tcp' and c['address']])")
udp_n=$(lim_json "len([c for c in d['servers'][0]['clients'] if c['transport']=='udp'])")
[ "$tcp_n" = 6 ] && [ "$udp_n" = 2 ]
check $? "6 clients on TCP (8 connections less 2 for pre-auth), both UDP ones too (tcp $tcp_n, udp $udp_n)"
docker logs "$P-lim" 2>&1 | grep -q "clients on TCP, the most this server can watch"
check $? "the seventh TCP client is refused with a reason"
docker logs "$P-lim" 2>&1 | grep -q "at most 8 of the 256 clients over TCP"
check $? "the log names the TCP limit at startup"

if [ "$FAILS" -ne 0 ]; then
    echo "FAIL: $FAILS check(s) failed"
    exit 1
fi
echo "PASS: a server on UDP and TCP at once, one pool"
