#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-e2e-reconfig.sh — a changed profile reaches a running client (#49)
#
# An auto client gets its address, MTU and routes from its [client ID]
# profile. The profile is changed and the server reloaded (SIGHUP); with the
# next handshake (a rekey every 5 s here, CAMEX_REKEY_AFTER) the client
# applies the new CONFIG to its live tunnel: new address, MTU and route,
# the old route gone, traffic on the new address. Over UDP and TCP. A
# tunnel fd from CAMEX_TUN_FD cannot be reopened: the change is refused
# and the fd stays open.
#
# Usage: ./test-e2e-reconfig.sh
#   CAMEX_BINARY=path/to/camex  binary to test (default: ../camex)
#   KEEP=1                      keep containers for debugging
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/camex}"
IMAGE="camex:e2e-reconfig"
P="camex-reconfig-$$"
NET_NAME="$P-net"
SRV="$P-srv"
CLI="$P-cli"
BUILD_CTX="$(mktemp -d)"
PSK="reconfig-test"
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
cat > "$BUILD_CTX/Dockerfile" <<'EOF'
FROM ubuntu:24.04
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
        iputils-ping iproute2 >/dev/null && rm -rf /var/lib/apt/lists/*
COPY camex /usr/local/bin/camex
COPY server.key /etc/camex/server.key
RUN printf '#!/bin/sh\nmkdir -p /dev/net\n[ -e /dev/net/tun ] || mknod /dev/net/tun c 10 200\nexec /usr/local/bin/camex "$@"\n' \
        > /usr/local/bin/camex-run && chmod +x /usr/local/bin/camex-run
ENTRYPOINT ["/usr/local/bin/camex-run"]
EOF
docker build -q -t "$IMAGE" "$BUILD_CTX" >/dev/null
docker network create "$NET_NAME" >/dev/null
set +e   # checks below report failures themselves

conf() {    # conf <last octet> <route> <mtu>: the client's profile
    printf '[server]\n[client CAM1]\nlocal_cidr=10.200.0.%s/24\ngateway_ip=10.200.0.1\nroute_cidr=%s\nmtu=%s\n' \
        "$1" "$2" "$3" > "$BUILD_CTX/camex.conf"
    docker cp "$BUILD_CTX/camex.conf" "$SRV:/etc/camex.conf" >/dev/null
}

for transport in ${TRANSPORTS:-udp tcp}; do
    echo "== $transport: a changed profile is applied without a restart"
    docker rm -f "$SRV" "$CLI" >/dev/null 2>&1
    docker create --name "$SRV" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
        "$IMAGE" --mode server --port 5800 --transport "$transport" \
        --local-cidr 10.200.0.1/24 $SERVER_KEY_ARGS --psk "$PSK" \
        --config /etc/camex.conf >/dev/null
    conf 50 192.168.150.0/24 1500
    docker start "$SRV" >/dev/null
    sleep 1
    docker run -d --name "$CLI" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
        -e CAMEX_REKEY_AFTER=5 \
        "$IMAGE" --mode client --auto --name CAM1 --transport "$transport" \
        --server-host "$SRV" --port 5800 \
        --server-pubkey "$SERVER_PUBKEY" --psk "$PSK" >/dev/null
    up=1
    for _ in $(seq 1 20); do
        docker exec "$SRV" ping -c1 -W1 10.200.0.50 >/dev/null 2>&1 && { up=0; break; }
        sleep 1
    done
    check $up "$transport: the client is up at 10.200.0.50"
    docker exec "$CLI" ip route | grep -q "^192.168.150.0/24"
    check $? "$transport: the profile's route is set"

    conf 60 192.168.160.0/24 1400
    docker kill -s HUP "$SRV" >/dev/null
    up=1
    for _ in $(seq 1 30); do
        docker exec "$SRV" ping -c1 -W1 10.200.0.60 >/dev/null 2>&1 && { up=0; break; }
        sleep 1
    done
    check $up "$transport: after the reload the server reaches the client at 10.200.0.60"
    docker exec "$CLI" ping -c3 -W1 10.200.0.1 >/dev/null 2>&1
    check $? "$transport: the client reaches the server from its new address"
    addrs=$(docker exec "$CLI" ip -4 -o addr show 2>/dev/null | grep "10\.200\.0\.")
    echo "$addrs" | grep -q "10.200.0.60/24" && ! echo "$addrs" | grep -q "10.200.0.50/"
    check $? "$transport: the tunnel has the new address only"
    docker exec "$CLI" ip -o link show 2>/dev/null | grep "tun" | grep -q "mtu 1400"
    check $? "$transport: the tunnel MTU is 1400"
    routes=$(docker exec "$CLI" ip route)
    echo "$routes" | grep -q "^192.168.160.0/24" && ! echo "$routes" | grep -q "^192.168.150.0/24"
    check $? "$transport: the new route replaced the old one"
    docker logs "$CLI" 2>&1 | grep -q "Applying the new tunnel configuration"
    check $? "$transport: the client logged the change"
    [ "$(docker inspect -f '{{.RestartCount}} {{.State.Running}}' "$CLI")" = "0 true" ]
    check $? "$transport: the client was not restarted"
done

# A TUN fd from CAMEX_TUN_FD (host application without a provider) cannot
# be reopened: the change is refused with a log line asking for a restart,
# and the fd stays open. perl (in the base image) opens the device, sets it up as the host
# application would and passes the fd to camex.
echo "== udp: CAMEX_TUN_FD keeps its tunnel when the profile changes"
docker rm -f "$SRV" "$CLI" >/dev/null 2>&1
docker create --name "$SRV" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    "$IMAGE" --mode server --port 5800 --transport udp \
    --local-cidr 10.200.0.1/24 $SERVER_KEY_ARGS --psk "$PSK" \
    --config /etc/camex.conf >/dev/null
conf 50 192.168.150.0/24 1500
docker start "$SRV" >/dev/null
sleep 1
docker run -d --name "$CLI" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    -e CAMEX_REKEY_AFTER=5 --entrypoint /usr/bin/perl \
    "$IMAGE" -e '
        mkdir "/dev/net"; system("mknod /dev/net/tun c 10 200") unless -e "/dev/net/tun";
        $^F = 255;   # keep the fd across exec
        open(my $t, "+<", "/dev/net/tun") or die "tun: $!";
        my $ifr = pack("a16 s x22", "camex0", 0x1001);   # IFF_TUN | IFF_NO_PI
        ioctl($t, 0x400454ca, $ifr) or die "TUNSETIFF: $!";
        system("ip addr add 10.200.0.50/24 dev camex0 && ip link set camex0 up") == 0 or die "ip";
        $ENV{CAMEX_TUN_FD} = fileno($t);
        exec("/usr/local/bin/camex", @ARGV) or die "exec: $!";' -- \
    --mode client --auto --name CAM1 --transport udp \
    --server-host "$SRV" --port 5800 \
    --server-pubkey "$SERVER_PUBKEY" --psk "$PSK" >/dev/null
up=1
for _ in $(seq 1 20); do
    docker exec "$SRV" ping -c1 -W1 10.200.0.50 >/dev/null 2>&1 && { up=0; break; }
    sleep 1
done
check $up "fd: the client is up at 10.200.0.50"
conf 60 192.168.160.0/24 1400
docker kill -s HUP "$SRV" >/dev/null
seen=1
for _ in $(seq 1 30); do
    docker logs "$CLI" 2>&1 | grep -q "CAMEX_TUN_FD cannot be reopened" && { seen=0; break; }
    sleep 1
done
check $seen "fd: the client refused to reopen the CAMEX_TUN_FD tunnel"
# The server has moved CAM1 to the new address, so traffic waits for a
# restart; the fd must stay open, not closed and left dangling. camex is
# the only holder of the fd, and a non-persistent TUN device goes away
# with its last fd.
docker exec "$CLI" ip link show camex0 >/dev/null 2>&1
check $? "fd: the client still holds the CAMEX_TUN_FD descriptor (camex0 exists)"
! docker logs "$CLI" 2>&1 | grep -q "Applying the new tunnel configuration"
check $? "fd: the tunnel was not closed"
[ "$(docker inspect -f '{{.RestartCount}} {{.State.Running}}' "$CLI")" = "0 true" ]
check $? "fd: the client is still running"

if [ "$FAILS" != 0 ]; then
    echo "--- client log (tail) ---"
    docker logs "$CLI" 2>&1 | tail -15
    echo "--- server log (tail) ---"
    docker logs "$SRV" 2>&1 | tail -10
    echo "FAIL: $FAILS check(s) failed"
    exit 2
fi
echo "PASS: a changed profile reaches running clients over UDP and TCP; a CAMEX_TUN_FD tunnel is kept"
