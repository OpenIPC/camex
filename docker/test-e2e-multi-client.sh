#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-e2e-multi-client.sh — real auto clients with different IDs never
# share an address (#81)
#
# 1-4. Three and four clients get different addresses, also after a client
#    restart (it gets its address back) and a server restart.
# 5. Right after a restart the pool waits for returning clients: a new
#    client that comes first does not take a returning client's address.
# 6. An expired client that is still alive keeps its address leased: a
#    full pool does not hand it out, and the client gets it back.
# 7. A manual client cannot claim a live auto client's address.
# 8. A reload that gives a live client's address to a [client ID] profile
#    ends that client's session; it comes back with another address.
# 9. With all slots taken, the evicted client's address stays leased.
# 10. A new TCP client that comes during the start hold reconnects until
#    it gets an address.
#
# Usage: ./test-e2e-multi-client.sh
#   CAMEX_BINARY=path/to/camex  binary to test (default: ../camex)
#   KEEP=1                      keep containers for debugging
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/camex}"
IMAGE="camex:e2e-multi"
P="camex-multi-$$"
NET_NAME="$P-net"
BUILD_CTX="$(mktemp -d)"
PSK="multi-test"
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
        iputils-ping iproute2 >/dev/null && rm -rf /var/lib/apt/lists/*
COPY camex /usr/local/bin/camex
COPY server.key /etc/camex/server.key
RUN printf '#!/bin/sh\nmkdir -p /dev/net\n[ -e /dev/net/tun ] || mknod /dev/net/tun c 10 200\nexec /usr/local/bin/camex "$@"\n' \
        > /usr/local/bin/camex-run && chmod +x /usr/local/bin/camex-run
ENTRYPOINT ["/usr/local/bin/camex-run"]
DOCKERFILE
docker build -q -t "$IMAGE" "$BUILD_CTX" >/dev/null 2>&1
docker network create "$NET_NAME" >/dev/null
set +e   # checks below report failures themselves

server() {  # server [docker args...]
    docker run -d --name "$P-srv" --network "$NET_NAME" --cap-add NET_ADMIN \
        --cap-add MKNOD "$@" "$IMAGE" --mode server --port 5800 \
        --local-cidr 10.200.0.1/24 $SERVER_KEY_ARGS --psk "$PSK" >/dev/null
}

client() {  # client <name> [camex args...]: an auto-mode client
    local name="$1"; shift
    docker run -d --name "$P-$name" --network "$NET_NAME" --cap-add NET_ADMIN \
        --cap-add MKNOD "$IMAGE" --mode client --auto --name "$name" \
        --server-host "$P-srv" --port 5800 --server-pubkey "$SERVER_PUBKEY" \
        --psk "$PSK" --server-timeout 5 "$@" >/dev/null
}

addr_of() {  # addr_of <container>: its tunnel address, or nothing
    docker exec "$P-$1" ip -4 -o addr show 2>/dev/null |
        awk '$2 != "lo" && $4 ~ /^10\.200\./ { sub("/.*", "", $4); print $4 }'
}

wait_addr() {  # wait_addr <container> <seconds>: its address once it has one
    local a _
    for _ in $(seq 1 "$2"); do
        a=$(addr_of "$1")
        [ -n "$a" ] && { echo "$a"; return 0; }
        sleep 1
    done
    return 1
}

unique() {  # unique <message> <container>...: different addresses, all reachable
    local msg="$1" c a all="" n ok=0
    shift
    for c in "$@"; do
        a=$(wait_addr "$c" 20)
        all="$all $c=${a:--}"
        [ -n "$a" ] || ok=1
    done
    n=$(for c in "$@"; do addr_of "$c"; done | sort | uniq -d | wc -l)
    [ "$ok" = 0 ] && [ "$n" = 0 ]
    check $? "$msg:$all"
    for c in "$@"; do
        a=$(addr_of "$c")
        [ -n "$a" ] && docker exec "$P-srv" ping -c 2 -W 2 "$a" >/dev/null 2>&1
        check $? "the server reaches $c at ${a:--}"
    done
}

echo "== 1. three clients with different IDs"
server
sleep 1
client CAM-A
client CAM-B
client CAM-C
sleep 3
unique "three different addresses" CAM-A CAM-B CAM-C

echo "== 2. a client restarts"
before=$(addr_of CAM-B)
docker restart "$P-CAM-B" >/dev/null
sleep 3
unique "still different" CAM-A CAM-B CAM-C
[ "$(addr_of CAM-B)" = "$before" ]
check $? "CAM-B gets its address back ($before -> $(addr_of CAM-B))"

echo "== 3. the server restarts"
docker restart "$P-srv" >/dev/null
sleep 12
unique "after the server restart" CAM-A CAM-B CAM-C

echo "== 4. a fourth client joins"
client CAM-D
sleep 3
unique "four different addresses" CAM-A CAM-B CAM-C CAM-D

reset() {   # remove the server and all clients
    docker ps -a --format '{{.Names}}' | grep "^$P-" | xargs -r docker rm -f >/dev/null 2>&1
}

server_conf() {  # server_conf <config text> [docker args...]: with a config file
    local conf="$1"; shift
    printf "$conf" > "$BUILD_CTX/srv.conf"
    docker create --name "$P-srv" --network "$NET_NAME" --cap-add NET_ADMIN \
        --cap-add MKNOD "$@" "$IMAGE" --mode server --port 5800 \
        --local-cidr 10.200.0.1/24 $SERVER_KEY_ARGS --psk "$PSK" \
        --config /etc/camex.conf >/dev/null
    docker cp "$BUILD_CTX/srv.conf" "$P-srv:/etc/camex.conf" >/dev/null
    docker start "$P-srv" >/dev/null
}

holders() {  # holders <address>: the containers that have it
    local c
    for c in $(docker ps --format '{{.Names}}' | grep "^$P-" | grep -v -- "-srv$"); do
        [ "$(addr_of "${c#$P-}")" = "$1" ] && echo "${c#$P-}"
    done
}

echo "== 5. after a restart the pool waits for returning clients"
reset
server -e CAMEX_POOL_HOLD=15
sleep 1
client CAM-A
client CAM-B
sleep 17
a_ip=$(wait_addr CAM-A 10); b_ip=$(wait_addr CAM-B 10)
echo "   CAM-A $a_ip, CAM-B $b_ip"
docker restart "$P-srv" >/dev/null
client NEW5
sleep 25
unique "after the restart" CAM-A CAM-B NEW5
[ "$(addr_of CAM-A)" = "$a_ip" ] && [ "$(addr_of CAM-B)" = "$b_ip" ]
check $? "the returning clients keep their addresses (CAM-A $(addr_of CAM-A), CAM-B $(addr_of CAM-B))"

echo "== 6. an expired client that is still alive keeps its address"
reset
# A /29: client addresses .2-.6
server_conf '[server]\nauto_pool=10.200.0.0/29\n' -e CAMEX_CLIENT_TIMEOUT=5
sleep 1
docker run -d --name "$P-SIL" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    -e CAMEX_REKEY_AFTER=40 "$IMAGE" --mode client --auto --name SIL \
    --server-host "$P-srv" --port 5800 --server-pubkey "$SERVER_PUBKEY" --psk "$PSK" \
    --keepalive 0 >/dev/null
s_ip=$(wait_addr SIL 20)
echo "   SIL has $s_ip"
gone=1
for _ in $(seq 1 25); do
    docker logs "$P-srv" 2>&1 | grep -q "Expired client $s_ip" && { gone=0; break; }
    sleep 1
done
check $gone "the server expires the silent client"
for n in 1 2 3 4 5; do client "N$n"; done
sleep 6
[ "$(holders "$s_ip")" = "SIL" ]
check $? "a full pool does not hand out its address ($s_ip: $(holders "$s_ip" | tr '\n' ' '))"
docker logs "$P-srv" 2>&1 | grep -q "Auto pool 10.200.0.0/29 is full"
check $? "the fifth new client finds the pool full"
docker run -d --name "$P-MANL" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD "$IMAGE" \
    --mode client --local-cidr "$s_ip/29" --gateway-ip 10.200.0.1 --server-host "$P-srv" \
    --port 5800 --server-pubkey "$SERVER_PUBKEY" --psk "$PSK" >/dev/null
sleep 4
docker logs "$P-srv" 2>&1 | grep -q "address $s_ip is kept for client SIL"
check $? "a manual client cannot claim the kept address"
docker rm -f "$P-MANL" >/dev/null
back=1
for _ in $(seq 1 50); do
    docker logs "$P-srv" 2>&1 | grep -q "Registered auto client SIL" &&
        [ "$(docker logs "$P-srv" 2>&1 | grep -c "Registered auto client SIL")" -ge 2 ] && { back=0; break; }
    sleep 1
done
[ "$back" = 0 ] && [ "$(addr_of SIL)" = "$s_ip" ] &&
    docker exec "$P-srv" ping -c 2 -W 2 "$s_ip" >/dev/null 2>&1
check $? "the client comes back with its address ($(addr_of SIL))"

echo "== 7. a manual client cannot claim a live auto client's address"
reset
server
sleep 1
client AUTO1
a_ip=$(wait_addr AUTO1 20)
docker run -d --name "$P-MAN" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD "$IMAGE" \
    --mode client --local-cidr "$a_ip/24" --gateway-ip 10.200.0.1 --server-host "$P-srv" \
    --port 5800 --server-pubkey "$SERVER_PUBKEY" --psk "$PSK" >/dev/null
sleep 5
docker logs "$P-srv" 2>&1 | grep -q "Rejected handshake from .*: address $a_ip is in use by client AUTO1" &&
    ! docker logs "$P-srv" 2>&1 | grep -q "Client $a_ip reconnected"
check $? "the server refuses the manual client"
docker exec "$P-AUTO1" ping -c 2 -W 2 10.200.0.1 >/dev/null 2>&1 &&
    ! docker exec "$P-MAN" ping -c 2 -W 2 10.200.0.1 >/dev/null 2>&1
check $? "the auto client keeps the address, the manual one has no tunnel"

echo "== 8. a reload gives a live client's address to a profile"
reset
server_conf '[server]\n'
sleep 1
client AUTO2
a_ip=$(wait_addr AUTO2 20)
printf '[server]\n[client PROF]\nlocal_cidr=%s/24\ngateway_ip=10.200.0.1\n' "$a_ip" > "$BUILD_CTX/c2.conf"
docker cp "$BUILD_CTX/c2.conf" "$P-srv:/etc/camex.conf" >/dev/null
docker kill -s HUP "$P-srv" >/dev/null
sleep 1
docker logs "$P-srv" 2>&1 | grep -q "Ended session of client AUTO2: its address $a_ip is now client PROF's"
check $? "the reload ends the session of the client on that address"
client PROF
moved=1
for _ in $(seq 1 45); do
    o=$(addr_of AUTO2)
    [ -n "$o" ] && [ "$o" != "$a_ip" ] && [ "$(addr_of PROF)" = "$a_ip" ] && { moved=0; break; }
    sleep 1
done
check $moved "the profile client gets $a_ip, the other one moves ($(addr_of AUTO2))"
unique "no shared address" AUTO2 PROF

echo "== 9. an evicted client's address stays leased"
reset
server_conf '[server]\nmax_clients=2\n'
sleep 1
client E1
e1=$(wait_addr E1 20)
client E2
wait_addr E2 20 >/dev/null
docker stop "$P-E1" >/dev/null   # quiet: it is the one evicted
sleep 2
client E3
e3=$(wait_addr E3 20)
[ -n "$e3" ] && [ "$e3" != "$e1" ]
check $? "the new client does not get the evicted client's address ($e1 -> $e3)"

echo "== 10. a new TCP client during the start hold"
reset
docker run -d --name "$P-srv" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    -e CAMEX_POOL_HOLD=15 "$IMAGE" --mode server --port 5800 --transport tcp \
    --local-cidr 10.200.0.1/24 $SERVER_KEY_ARGS --psk "$PSK" >/dev/null
sleep 1
client NEWT --transport tcp
t_ip=$(wait_addr NEWT 45)
[ -n "$t_ip" ] && docker exec "$P-srv" ping -c 2 -W 2 "$t_ip" >/dev/null 2>&1
check $? "it reconnects and gets an address after the hold (${t_ip:--})"

if [ "$FAILS" -ne 0 ]; then
    echo "FAIL: $FAILS check(s) failed"
    exit 1
fi
echo "PASS: auto clients with different IDs never share an address"
