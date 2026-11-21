#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-e2e-bridge.sh — a TAP network bridged to a LAN (--bridge, the full
# camex): no bridge device, no TAP device on the bridging host
#
#   wan:  server, remote client (TAP), the bridging host's uplink
#   lan:  the bridging host's eth1, a LAN device running a DHCP server
#
#  1. The remote client gets its address from the LAN's DHCP server
#  2. Remote <-> LAN device, both ways
#  3. Remote <-> the bridging host, both ways (its stack gets the packets
#     through a raw socket; it learns the remote's MAC as a neighbour)
#  4. 8 MB over TCP each way (offloads: checksums, TSO/GRO segments)
#  5. The bridging host's settings are untouched: no bridge, no TAP, no
#     promiscuous flag left, and no neighbour entry after it stops
#  6. A second bridge of the network on the same LAN stops bridging
#  7. nat mode (as on a Wi-Fi station): DHCP, the LAN device, the host
#
# Usage: ./test-e2e-bridge.sh
#   CAMEX_BINARY=path/to/camex           any camex for the keys (default ../camex)
#   CAMEX_FULL_BINARY=path/to/camex-full the full one (default: built here)
#   KEEP=1                               keep containers for debugging
#

set -e
export LC_ALL=C

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/camex}"
IMAGE="camex:e2e-bridge"
P="camex-br-$$"
WAN="$P-wan"
LAN="$P-lan"
BUILD_CTX="$(mktemp -d)"
FAILS=0

cleanup() {
    docker ps -a --format '{{.Names}}' | grep "^$P-" | xargs -r docker rm -f >/dev/null 2>&1 || true
    docker network rm "$WAN" "$LAN" >/dev/null 2>&1 || true
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
cp "$CAMEX_FULL_BINARY" "$BUILD_CTX/camex"
. "$SCRIPT_DIR/lib-keys.sh"
gen_server_key "$BUILD_CTX" "$CAMEX_BINARY" || { echo "cannot create a server key"; exit 1; }
cat > "$BUILD_CTX/Dockerfile" <<'DOCKERFILE'
FROM ubuntu:24.04
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
        iputils-ping iproute2 busybox dnsmasq-base python3-minimal >/dev/null && \
    rm -rf /var/lib/apt/lists/*
COPY camex /usr/local/bin/camex
COPY server.key /etc/camex/server.key
RUN printf '#!/bin/sh\nmkdir -p /dev/net\n[ -e /dev/net/tun ] || mknod /dev/net/tun c 10 200\nexec /usr/local/bin/camex "$@"\n' \
        > /usr/local/bin/camex-run && chmod +x /usr/local/bin/camex-run && \
    printf '#!/bin/sh\n[ "$1" = bound ] || [ "$1" = renew ] || exit 0\nip addr flush dev $interface\nip addr add $ip/${mask:-24} dev $interface\n' \
        > /usr/local/bin/udhcpc-script && chmod +x /usr/local/bin/udhcpc-script
ENTRYPOINT ["/usr/local/bin/camex-run"]
DOCKERFILE
docker build -q -t "$IMAGE" "$BUILD_CTX" >/dev/null 2>&1
docker network create "$WAN" >/dev/null
docker network create --subnet 192.168.77.0/24 "$LAN" >/dev/null
set +e

cat > "$BUILD_CTX/camex.conf" <<'EOF'
[server]
local_cidr = 10.200.0.1/24

[network site]
weak_name = yes
mode = tap
addresses = none
EOF
docker create --name "$P-srv" --network "$WAN" --cap-add NET_ADMIN --cap-add MKNOD \
    "$IMAGE" --mode server --port 5800 $SERVER_KEY_ARGS --config /etc/camex.conf >/dev/null
docker cp "$BUILD_CTX/camex.conf" "$P-srv:/etc/camex.conf" >/dev/null
docker start "$P-srv" >/dev/null

# The LAN device: a DHCP server for 192.168.77.100-150
docker run -d --name "$P-dev" --network "$LAN" --ip 192.168.77.10 --cap-add NET_ADMIN \
    --entrypoint dnsmasq "$IMAGE" -d -i eth0 --dhcp-range=192.168.77.100,192.168.77.150,255.255.255.0,1h \
    --port=0 >/dev/null

bridge_host() {  # bridge_host <name> <ip on lan> [camex args...]: on wan, eth1 on lan;
                 # camex is not the container's main process, so it can stop
                 # while the container stays (its log: /tmp/camex.log)
    local name="$1" ip="$2"; shift 2
    docker create --name "$P-$name" --network "$WAN" --cap-add NET_ADMIN --cap-add MKNOD \
        --entrypoint sleep "$IMAGE" infinity >/dev/null
    docker network connect --ip "$ip" "$LAN" "$P-$name"
    docker start "$P-$name" >/dev/null
    # Docker does not keep the order of the networks: the LAN interface is
    # the one with the LAN address (eth0 or eth1)
    local lan_if
    lan_if=$(docker exec "$P-$name" ip -o -4 addr show | awk -v ip="$ip/" 'index($4, ip) == 1 { print $2 }')
    docker exec "$P-$name" sh -c "echo $lan_if > /tmp/lan_if"
    docker exec -d "$P-$name" sh -c "exec /usr/local/bin/camex-run --server-host $P-srv \
        --network site --bridge $lan_if $* > /tmp/camex.log 2>&1"
}
blog() {    # blog <name>: the log of camex in a bridge host
    docker exec "$P-$1" cat /tmp/camex.log 2>/dev/null
}
bridge_host br 192.168.77.2 --name BR-1
for _ in $(seq 1 30); do
    blog br | grep -q "bridging$" && break
    sleep 0.5
done
blog br | grep -q "Bridge on $(docker exec "$P-br" cat /tmp/lan_if)"
check $? "the bridge is up on eth1"

docker run -d --name "$P-rem" --network "$WAN" --cap-add NET_ADMIN --cap-add MKNOD \
    "$IMAGE" --server-host "$P-srv" --network site --name REMOTE >/dev/null
for _ in $(seq 1 60); do
    docker exec "$P-rem" sh -c 'ls /sys/class/net | grep -q tap' && break
    sleep 0.5
done

echo "== 1. DHCP from the LAN"
for _ in 1 2 3; do
    docker exec "$P-rem" busybox udhcpc -i tap0 -n -q -t 5 -s /usr/local/bin/udhcpc-script >/dev/null 2>&1 && break
done
R=$(docker exec "$P-rem" ip -4 -o addr show tap0 | awk '{ print $4 }')
case "$R" in 192.168.77.1[0-4][0-9]/24|192.168.77.150/24) r=0 ;; *) r=1 ;; esac
check $r "the remote has a LAN address from the LAN's DHCP server: $R"
if [ $r != 0 ]; then   # what was going on
    blog br | grep -v "^ " | sed 's/^/   br: /'
    docker logs "$P-rem" 2>&1 | grep -v "^ " | tail -5 | sed 's/^/   rem: /'
    docker exec "$P-srv" camex --status 2>&1 | tail -4 | sed 's/^/   srv: /'
    docker logs "$P-dev" 2>&1 | tail -6 | sed 's/^/   dev: /'
    docker exec "$P-br" ip neigh | sed 's/^/   br-neigh: /'
fi
RIP=${R%/*}

echo "== 2. remote <-> LAN device"
docker exec "$P-rem" ping -c 2 -W 2 192.168.77.10 >/dev/null
check $? "the remote reaches the LAN device"
docker exec "$P-dev" ping -c 2 -W 2 "$RIP" >/dev/null
check $? "the LAN device reaches the remote"

echo "== 3. remote <-> the bridging host"
docker exec "$P-rem" ping -c 2 -W 2 192.168.77.2 >/dev/null
check $? "the remote reaches the bridging host"
docker exec "$P-br" ping -c 2 -W 2 "$RIP" >/dev/null
check $? "the bridging host reaches the remote"
docker exec "$P-br" sh -c "ip neigh show $RIP dev \$(cat /tmp/lan_if)" | grep -q PERMANENT
check $? "the host has the remote as a neighbour entry"

echo "== 4. 8 MB over TCP each way"
SRV='import socket,sys,hashlib
s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1); s.bind(("",9000)); s.listen(1)
c,_=s.accept(); h=hashlib.sha256(); n=0
while True:
    d=c.recv(65536)
    if not d: break
    h.update(d); n+=len(d)
print(n, h.hexdigest()[:16])'
CLI='import socket,sys,hashlib
data=bytes((i*7+3)&255 for i in range(8*1024*1024))
s=socket.create_connection((sys.argv[1],9000),timeout=30); s.sendall(data); s.close()
print(len(data), hashlib.sha256(data).hexdigest()[:16])'
tcp_check() {   # tcp_check <receiver> <sender> <receiver ip> <what>
    docker exec "$P-$1" timeout 70 python3 -c "$SRV" > "$BUILD_CTX/rx.txt" 2>&1 &
    local rx=$!
    sleep 1
    docker exec "$P-$2" timeout 60 python3 -c "$CLI" "$3" > "$BUILD_CTX/tx.txt" 2>&1
    wait $rx
    [ -s "$BUILD_CTX/rx.txt" ] && cmp -s "$BUILD_CTX/rx.txt" "$BUILD_CTX/tx.txt"
    check $? "$4: $(cat "$BUILD_CTX/rx.txt")"
}
tcp_check br rem 192.168.77.2 "remote -> bridging host"
tcp_check rem br "$RIP" "bridging host -> remote (TSO, checksum offload)"
tcp_check dev rem 192.168.77.10 "remote -> LAN device"
tcp_check rem dev "$RIP" "LAN device -> remote (GRO on the bridge)"

echo "== 5. the host's settings"
docker exec "$P-br" sh -c 'ls /sys/class/net' | grep -Eq "^(tap|tun|br|camex)"
r=$?; [ $r != 0 ]; check $? "no TAP, TUN or bridge device on the bridging host"
docker exec "$P-br" sh -c 'ip neigh show dev $(cat /tmp/lan_if)' | grep -q PERMANENT
check $? "while camex runs the remote is a permanent neighbour"
docker exec "$P-br" sh -c 'pkill -x camex; sleep 2; ip neigh show dev $(cat /tmp/lan_if)' > "$BUILD_CTX/neigh.txt" 2>&1
docker exec "$P-br" true
check $? "the bridge host is still there"
! grep -q PERMANENT "$BUILD_CTX/neigh.txt"
check $? "no permanent neighbour entry left after camex stops"
docker exec "$P-br" sh -c 'ip link show $(cat /tmp/lan_if)' > "$BUILD_CTX/link.txt" 2>&1
grep -q "eth" "$BUILD_CTX/link.txt" && ! grep -q PROMISC "$BUILD_CTX/link.txt"
check $? "the LAN interface is not left promiscuous"
docker rm -f "$P-br" >/dev/null
bridge_host br 192.168.77.2 --name BR-1
for _ in $(seq 1 30); do
    blog br | grep -q "bridging$" && break
    sleep 0.5
done

echo "== 6. two bridges of the network on one LAN"
bridge_host br2 192.168.77.3 --name BR-2
for _ in $(seq 1 40); do
    blog br2 | grep -q "client BR-1 bridges this network on the same LAN" && break
    sleep 0.5
done
blog br2 | grep -q "client BR-1 bridges this network on the same LAN"
check $? "the second bridge (BR-2) stops: BR-1 bridges there"
docker exec "$P-rem" ping -c 2 -W 2 192.168.77.10 >/dev/null
check $? "the remote still reaches the LAN device (no loop)"
docker rm -f "$P-br2" >/dev/null

echo "== 7. nat mode (as on a Wi-Fi station)"
docker rm -f "$P-br" >/dev/null
bridge_host br 192.168.77.2 --name BR-1 --bridge-mode nat
for _ in $(seq 1 30); do
    blog br | grep -q "bridging$" && break
    sleep 0.5
done
blog br | grep -q "nat mode"
check $? "the bridge is in nat mode"
docker exec "$P-rem" sh -c 'ip addr flush dev tap0'
docker exec "$P-rem" busybox udhcpc -i tap0 -n -q -t 5 -s /usr/local/bin/udhcpc-script >/dev/null 2>&1
R=$(docker exec "$P-rem" ip -4 -o addr show tap0 | awk '{ print $4 }')
case "$R" in 192.168.77.*/24) r=0 ;; *) r=1 ;; esac
check $r "nat: DHCP from the LAN: $R"
RIP=${R%/*}
docker exec "$P-rem" ping -c 2 -W 2 192.168.77.10 >/dev/null
check $? "nat: the remote reaches the LAN device"
docker exec "$P-dev" ping -c 2 -W 2 "$RIP" >/dev/null
check $? "nat: the LAN device reaches the remote"
docker exec "$P-dev" ip neigh show "$RIP" | grep -qi "$(docker exec "$P-br" sh -c 'cat /sys/class/net/$(cat /tmp/lan_if)/address')"
check $? "nat: the LAN knows the remote by the bridging host's MAC"
docker exec "$P-rem" ping -c 2 -W 2 192.168.77.2 >/dev/null
check $? "nat: the remote reaches the bridging host"
tcp_check dev rem 192.168.77.10 "nat: remote -> LAN device"

echo
if [ "$FAILS" = 0 ]; then
    echo "test-e2e-bridge: all checks passed"
else
    echo "test-e2e-bridge: $FAILS check(s) FAILED"
    exit 1
fi
