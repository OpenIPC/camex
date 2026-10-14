#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-e2e-key-binding.sh — regression test for issues #5 and #6
#
# Server config: CAMA and CAMB have their own keys, CAMC has none (uses
# the global key). Checks:
#   1. CAMA (own key) and CAMC (global key) get their configs.
#   2. CAMB's key cannot register as CAMA                         (#5)
#   3. The global key cannot register as CAMA, which has its own  (#5)
#   4. CAMB cannot send packets with CAMA's tunnel address        (#6)
#   5. A manual client with CAMB's key cannot claim CAMA's address
#      while CAMA is offline
#   6. A manual client with the global key cannot claim the address of
#      CAMD, which comes from the auto pool
#      (its section has a key but no local_cidr)
#   7. CAME, whose own psk equals the global one, can still register
# After each attack the victim must still reach the server.
#
# Usage: ./test-e2e-key-binding.sh
#   CAMEX_BINARY=path/to/camex  binary to test (default: ../camex)
#   KEEP=1                      keep containers for debugging
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/camex}"
IMAGE="camex:e2e-keys"
P="camex-keys-$$"
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
cat > "$BUILD_CTX/camex.conf" <<'EOF'
[server]
[client CAMA]
local_cidr=10.200.0.10/24
gateway_ip=10.200.0.1
psk=key-a
[client CAMB]
local_cidr=10.200.0.11/24
gateway_ip=10.200.0.1
psk=key-b
[client CAMC]
local_cidr=10.200.0.12/24
gateway_ip=10.200.0.1
[client CAMD]
psk=key-d
[client CAME]
local_cidr=10.200.0.13/24
gateway_ip=10.200.0.1
psk=global-key
EOF
docker build -q -t "$IMAGE" "$BUILD_CTX" >/dev/null
docker network create "$NET_NAME" >/dev/null
set +e   # checks below report failures themselves

run() {     # run <name> <camex args...>
    local name="$P-$1"; shift
    docker run -d --name "$name" --network "$NET_NAME" --cap-add NET_ADMIN \
        --cap-add MKNOD --cap-add NET_RAW "$IMAGE" "$@" >/dev/null
}
client() {  # client <name> <camex client args...>
    local name="$1"; shift
    run "$name" --mode client --server-host "$SRV" --port 5800 --encrypt "$@"
}
got_config() { docker logs "$P-$1" 2>&1 | grep -q "Received config from server"; }
still_up() {  # still_up <name> <who>: the victim keeps its session
    docker exec "$P-$1" ping -c2 -W2 10.200.0.1 >/dev/null 2>&1
    check $? "$2 still reaches the server"
}
icmp_in() {  # ICMP echo requests received by a container
    docker exec "$1" awk '/^Icmp:/ { if (n++) print $10 }' /proc/net/snmp
}

echo "== server"
docker create --name "$SRV" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    "$IMAGE" --mode server --port 5800 --local-cidr 10.200.0.1/24 \
    --encrypt --psk global-key --config /etc/camex/camex.conf >/dev/null
mkdir -p "$BUILD_CTX/etc-camex" && cp "$BUILD_CTX/camex.conf" "$BUILD_CTX/etc-camex/"
docker cp "$BUILD_CTX/etc-camex" "$SRV:/etc/camex" >/dev/null
docker start "$SRV" >/dev/null
sleep 1

echo "== 1. legitimate clients"
client a --auto --name CAMA --psk key-a
client c --auto --name CAMC --psk global-key
sleep 6
got_config a; check $? "CAMA (own key) gets its config"
got_config c; check $? "CAMC (global key, no own key) gets its config"
docker exec "$P-a" ping -c2 -W1 10.200.0.1 >/dev/null 2>&1; check $? "CAMA reaches the server"

echo "== 2. CAMB's key registers as CAMA"
client x1 --auto --name CAMA --psk key-b
sleep 6
! got_config x1; check $? "impersonation with another client's key is rejected"
docker rm -f "$P-x1" >/dev/null
still_up a CAMA

echo "== 3. global key registers as CAMA (which has its own key)"
client x2 --auto --name CAMA --psk global-key
sleep 6
! got_config x2; check $? "global key cannot register a client that has its own key"
docker rm -f "$P-x2" >/dev/null
still_up a CAMA

echo "== 4. CAMB sends packets with CAMA's tunnel address"
client b --auto --name CAMB --psk key-b
sleep 6
got_config b; check $? "CAMB (own key) gets its config"
before=$(icmp_in "$SRV")
docker exec "$P-b" python3 -c '
import socket, struct
def csum(b):
    s = sum(struct.unpack("!%dH" % (len(b) // 2), b))
    s = (s >> 16) + (s & 0xffff); s += s >> 16
    return ~s & 0xffff
icmp = struct.pack("!BBHHH", 8, 0, 0, 0x4242, 1)
icmp = icmp[:2] + struct.pack("!H", csum(icmp)) + icmp[4:]
src, dst = socket.inet_aton("10.200.0.10"), socket.inet_aton("10.200.0.1")
ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 28, 1, 0, 64, 1, 0, src, dst)
ip = ip[:10] + struct.pack("!H", csum(ip)) + ip[12:]
s = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_RAW)
s.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, b"tun0")
for _ in range(5):
    s.sendto(ip + icmp, ("10.200.0.1", 0))
'
sleep 2
after=$(icmp_in "$SRV")
[ "$after" = "$before" ]; check $? "spoofed source address does not reach the server (echo requests: $before -> $after)"
still_up a CAMA

echo "== 5. manual client with CAMB's key claims CAMA's address while CAMA is offline"
docker pause "$P-a" >/dev/null   # e.g. the camera lost its uplink
client x3 --local-cidr 10.200.0.10/24 --gateway-ip 10.200.0.1 --psk key-b
sleep 3
before=$(icmp_in "$P-x3")
docker exec "$SRV" ping -c5 -i 0.5 -W1 10.200.0.10 >/dev/null 2>&1
after=$(icmp_in "$P-x3")
docker unpause "$P-a" >/dev/null
[ "$after" = "$before" ]; check $? "CAMA's address is not taken over (server pings that reached the impostor: $((after - before)) of 5)"
docker rm -f "$P-x3" >/dev/null
sleep 3   # let CAMA's next keepalive go out after the pause
still_up a CAMA

echo "== 6. manual client with the global key claims CAMD's pool address while CAMD is offline"
client d --auto --name CAMD --psk key-d
sleep 6
got_config d; check $? "CAMD (own key, pool address) gets its config"
d_ip=$(docker logs "$SRV" 2>&1 | sed -n 's|.*Auto-assigned \([0-9.]*\)/.* to client CAMD.*|\1|p' | tail -1)
echo "   CAMD has ${d_ip:-no address}"
docker pause "$P-d" >/dev/null
client x4 --local-cidr "${d_ip:-10.200.0.2}/24" --gateway-ip 10.200.0.1 --psk global-key
sleep 3
before=$(icmp_in "$P-x4")
docker exec "$SRV" ping -c5 -i 0.5 -W1 "${d_ip:-10.200.0.2}" >/dev/null 2>&1
after=$(icmp_in "$P-x4")
docker unpause "$P-d" >/dev/null
[ -n "$d_ip" ] && [ "$after" = "$before" ]; check $? "CAMD's pool address is not taken over (server pings that reached the impostor: $((after - before)) of 5)"
docker rm -f "$P-x4" >/dev/null
sleep 3
still_up d CAMD

echo "== 7. CAME's own key is the same as the global key"
client e --auto --name CAME --psk global-key
sleep 6
got_config e; check $? "CAME (own key equal to the global key) gets its config"

if [ "$FAILS" -ne 0 ]; then
    echo "--- server log (tail) ---"
    docker logs "$SRV" 2>&1 | tail -15
    echo "FAIL: $FAILS check(s) failed"
    exit 2
fi
echo "PASS: keys are bound to client IDs and tunnel addresses cannot be spoofed"
