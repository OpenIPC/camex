#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-e2e-mesh.sh — mesh networks (mesh = yes, the full camex): peers go
# direct where their NATs let them, through the server where they do not
#
#   inet: 100.64.0.0/24, the server and the core router (.3)
#   transitN: 100.65.N.0/24, the core router (.3) and NAT router N (.2):
#            a hop between the NATs, as on the internet
#   siteN: 192.168.N.0/24 behind NAT router N (iptables MASQUERADE)
#
#  1. Two peers behind ordinary (cone-like) NATs: a direct path; a big
#     transfer between them hardly touches the server
#  2. Two peers behind symmetric NATs (MASQUERADE --random-fully): the
#     punches fail, they stay on the server and still talk
#  3. Two peers behind one NAT: direct over their LAN
#  4. A direct path that dies (the routers drop it): back to the server
#     within MESH_DEAD seconds, traffic goes on
#  5. camex --status --json and --top: relayed pairs, reported peers
#  6. A TAP mesh network: two TAP clients go direct
#
# Usage: ./test-e2e-mesh.sh
#   CAMEX_BINARY=path/to/camex           any camex for the keys (default ../camex)
#   CAMEX_FULL_BINARY=path/to/camex-full the full one (default: CAMEX_BINARY
#                                        when it has l2 and mesh, else built here)
#   KEEP=1                               keep containers for debugging
#

set -e
export LC_ALL=C

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/camex}"
IMAGE="camex:e2e-mesh"
P="camex-mesh-$$"
BUILD_CTX="$(mktemp -d)"
FAILS=0

cleanup() {
    docker ps -a --format '{{.Names}}' | grep "^$P-" | xargs -r docker rm -f >/dev/null 2>&1 || true
    docker network ls --format '{{.Name}}' | grep "^$P-" | xargs -r docker network rm >/dev/null 2>&1 || true
    rm -rf "$BUILD_CTX"
}
[ -n "$KEEP" ] || trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

check() {   # check <condition-exit-code> <message>
    if [ "$1" = 0 ]; then echo "   ok: $2"; else echo "   FAIL: $2"; FAILS=$((FAILS + 1)); fi
}

[ -x "$CAMEX_BINARY" ] || { echo "camex binary not found: $CAMEX_BINARY"; exit 1; }
# Since #204 the default build is the full one: --version says what it has
if [ -z "$CAMEX_FULL_BINARY" ] &&
   "$CAMEX_BINARY" --version 2>/dev/null | grep -q '^features:.* server.* l2.* mesh'; then
    CAMEX_FULL_BINARY=$CAMEX_BINARY
fi
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
        iputils-ping iproute2 iptables python3-minimal >/dev/null && \
    rm -rf /var/lib/apt/lists/*
COPY camex /usr/local/bin/camex
COPY server.key /etc/camex/server.key
RUN printf '#!/bin/sh\nmkdir -p /dev/net\n[ -e /dev/net/tun ] || mknod /dev/net/tun c 10 200\n[ -n "$GW" ] && ip route replace default via $GW\nexec /usr/local/bin/camex "$@"\n' \
        > /usr/local/bin/camex-run && chmod +x /usr/local/bin/camex-run
ENTRYPOINT ["/usr/local/bin/camex-run"]
DOCKERFILE
docker build -q -t "$IMAGE" "$BUILD_CTX" >/dev/null 2>&1
docker network create --internal --subnet 100.64.0.0/24 "$P-inet" >/dev/null
set +e

cat > "$BUILD_CTX/camex.conf" <<'EOF'
[server]
local_cidr = 10.200.0.1/24

[network mesh]
weak_name = yes
mesh = yes
pool = 10.90.0.0/24

[network tapmesh]
weak_name = yes
mesh = yes
mode = tap
pool = 10.91.0.0/24
EOF
# The core router: inet and every transit network, routing, no NAT
docker run -d --name "$P-core" --network "$P-inet" --ip 100.64.0.3 --cap-add NET_ADMIN \
    --sysctl net.ipv4.ip_forward=1 --entrypoint sleep "$IMAGE" infinity >/dev/null
docker create --name "$P-srv" --network "$P-inet" --ip 100.64.0.10 --cap-add NET_ADMIN \
    --cap-add MKNOD -e GW=100.64.0.3 "$IMAGE" --mode server --port 5800 $SERVER_KEY_ARGS \
    --config /etc/camex.conf >/dev/null
docker cp "$BUILD_CTX/camex.conf" "$P-srv:/etc/camex.conf" >/dev/null
docker start "$P-srv" >/dev/null

site() {    # site <n> <cone|symmetric>: network 192.168.n.0/24 and its NAT router
    local n="$1" kind="$2" rnd=""
    [ "$kind" = symmetric ] && rnd="--random-fully"
    docker network create --internal --subnet "192.168.$n.0/24" "$P-site$n" >/dev/null
    docker network create --internal --subnet "100.65.$n.0/24" "$P-transit$n" >/dev/null
    docker network connect --ip "100.65.$n.3" "$P-transit$n" "$P-core"
    docker run -d --name "$P-r$n" --network "$P-site$n" --ip "192.168.$n.2" \
        --cap-add NET_ADMIN --sysctl net.ipv4.ip_forward=1 \
        --entrypoint sleep "$IMAGE" infinity >/dev/null
    docker network connect --ip "100.65.$n.2" "$P-transit$n" "$P-r$n"
    docker exec "$P-r$n" sh -c "
        ip route replace default via 100.65.$n.3
        out=\$(ip -o -4 addr show | awk '\$4 ~ /^100\\.65\\./ { print \$2 }')
        iptables -t nat -A POSTROUTING -o \$out -j MASQUERADE $rnd"
}

peer() {    # peer <name> <site> <network> [camex args...]
    local name="$1" n="$2" net="$3"; shift 3
    docker run -d --name "$P-$name" --network "$P-site$n" --cap-add NET_ADMIN \
        --cap-add MKNOD -e GW="192.168.$n.2" "$IMAGE" --server-host 100.64.0.10 \
        --network "$net" --name "$name" --keepalive 5 "$@" >/dev/null
}

addr_of() { # addr_of <container> <prefix>: its tunnel address
    local i a
    for i in $(seq 1 40); do
        a=$(docker exec "$1" ip -4 -o addr show 2>/dev/null |
            awk -v p="$2" 'index($4, p) == 1 { split($4, x, "/"); print x[1]; exit }')
        [ -n "$a" ] && { echo "$a"; return 0; }
        sleep 0.5
    done
    return 1
}

wait_log() {    # wait_log <container> <regex> [seconds]
    local i
    for i in $(seq 1 $((${3:-10} * 2))); do
        docker logs "$1" 2>&1 | grep -Eq "$2" && return 0
        sleep 0.5
    done
    return 1
}

relayed_bytes() {   # relayed_bytes <from> <to>: what the server relayed
    docker exec "$P-srv" camex --status --json 2>/dev/null | python3 -c '
import json, sys
s = json.load(sys.stdin)["servers"][0]
print(sum(r["bytes"] for r in s.get("relayed", []) if r["from"] == sys.argv[1] and r["to"] == sys.argv[2]))
' "$1" "$2" 2>/dev/null || echo 0
}

SRV='import socket,sys
s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1); s.bind(("",9000)); s.listen(1)
c,_=s.accept(); n=0
while True:
    d=c.recv(65536)
    if not d: break
    n+=len(d)
print(n)'
CLI='import socket,sys
s=socket.create_connection((sys.argv[1],9000),timeout=30); s.sendall(bytes(4*1024*1024)); s.close()'

echo "== 1. two peers behind ordinary NATs"
site 1 cone
site 2 cone
peer A 1 mesh
peer B 2 mesh
A=$(addr_of "$P-A" 10.90.0.)
B=$(addr_of "$P-B" 10.90.0.)
check $? "A $A, B $B"
docker exec "$P-A" ping -c 3 -W 2 "$B" >/dev/null
check $? "A reaches B (through the server at first)"
wait_log "$P-A" "Mesh: peer B direct at 100\\.65\\.2\\.2:" 15
check $? "A has a direct path to B (through B's NAT)"
wait_log "$P-B" "Mesh: peer A direct at 100\\.65\\.1\\.2:" 5
check $? "B has a direct path to A"
before=$(relayed_bytes A B)
docker exec "$P-B" timeout 60 python3 -c "$SRV" > "$BUILD_CTX/rx.txt" 2>&1 &
rx=$!
sleep 1
docker exec "$P-A" timeout 60 python3 -c "$CLI" "$B" >/dev/null 2>&1
wait $rx
[ "$(cat "$BUILD_CTX/rx.txt")" = 4194304 ]; check $? "4 MB from A to B ($(cat "$BUILD_CTX/rx.txt"))"
sleep 2
after=$(relayed_bytes A B)
[ $((after - before)) -lt 200000 ]
check $? "the server relayed $((after - before)) bytes of it (direct path)"

echo "== 2. two peers behind symmetric NATs"
site 3 symmetric
site 4 symmetric
peer C 3 mesh
peer D 4 mesh
C=$(addr_of "$P-C" 10.90.0.)
D=$(addr_of "$P-D" 10.90.0.)
docker exec "$P-C" ping -c 3 -W 2 "$D" >/dev/null
check $? "C reaches D"
wait_log "$P-C" "Mesh: peer D stays on the server: no answer to punches" 25
check $? "C and D stay on the server (no direct path through symmetric NATs)"
docker exec "$P-C" ping -c 3 -W 2 "$D" >/dev/null
check $? "C still reaches D, through the server"
[ "$(relayed_bytes C D)" -gt 0 ]; check $? "the server relays C -> D"

echo "== 3. two peers behind one NAT"
peer E 1 mesh
E=$(addr_of "$P-E" 10.90.0.)
docker exec "$P-E" ping -c 3 -W 2 "$A" >/dev/null
check $? "E reaches A"
wait_log "$P-E" "Mesh: peer A direct at 192\\.168\\.1\\." 15
check $? "E and A go direct over their LAN"

echo "== 4. a direct path that dies"
docker exec "$P-core" iptables -I FORWARD -s 100.65.1.2 -d 100.65.2.2 -j DROP
docker exec "$P-core" iptables -I FORWARD -s 100.65.2.2 -d 100.65.1.2 -j DROP
# Only the peers' own path: the server still passes
for i in $(seq 1 25); do
    docker exec "$P-A" ping -c 1 -W 1 "$B" >/dev/null 2>&1
    sleep 2
    docker logs "$P-A" 2>&1 | grep -q "Mesh: peer B back through the server" && break
done
docker logs "$P-A" 2>&1 | grep -q "Mesh: peer B back through the server"
check $? "A goes back through the server when the direct path dies"
docker exec "$P-A" ping -c 3 -W 2 "$B" >/dev/null
check $? "A still reaches B"
docker exec "$P-core" iptables -D FORWARD -s 100.65.1.2 -d 100.65.2.2 -j DROP
docker exec "$P-core" iptables -D FORWARD -s 100.65.2.2 -d 100.65.1.2 -j DROP

echo "== 5. the server's view"
sleep 31   # the clients' STATS come every 30 s
docker exec "$P-srv" camex --status --json > "$BUILD_CTX/st.json" 2>/dev/null
python3 -c '
import json, sys
s = json.load(open(sys.argv[1]))["servers"][0]
peers = s["peers"]
assert any(p["client"] == "C" and p["peer"] == "D" and p["state"] == "relay" for p in peers), peers
assert any(p["client"] == "E" and p["peer"] == "A" and p["state"] == "direct" for p in peers), peers
assert any(r["from"] == "C" and r["to"] == "D" for r in s["relayed"]), s["relayed"]
' "$BUILD_CTX/st.json"
check $? "--status --json: relayed C -> D, E -> A direct, C -> D relay"
docker exec "$P-srv" sh -c 'timeout 3 camex --top --interval 1 </dev/null' > "$BUILD_CTX/top.txt" 2>&1
grep -Eq "Mesh +E +<-> +A|Mesh +A +<-> +E" "$BUILD_CTX/top.txt" &&
    grep -Eq "Mesh +C +<-> +D +relayed" "$BUILD_CTX/top.txt"
check $? "--top lists the peers the clients report"

echo "== 6. a TAP mesh network"
peer TA 1 tapmesh
peer TB 2 tapmesh
TA=$(addr_of "$P-TA" 10.91.0.)
TB=$(addr_of "$P-TB" 10.91.0.)
docker exec "$P-TA" ping -c 3 -W 2 "$TB" >/dev/null
check $? "TA reaches TB"
for i in $(seq 1 10); do
    docker exec "$P-TA" ping -c 1 -W 1 "$TB" >/dev/null 2>&1
    docker logs "$P-TA" 2>&1 | grep -q "Mesh: peer TB direct" && break
    sleep 1
done
docker logs "$P-TA" 2>&1 | grep -q "Mesh: peer TB direct"
check $? "TA and TB go direct"
docker exec "$P-TA" ping -c 3 -W 2 -M do -s 1472 "$TB" >/dev/null
check $? "1500-byte packets on the direct TAP path"

echo "== 7. the server gone: direct peers keep talking"
docker exec "$P-E" ping -c 3 -W 2 "$A" >/dev/null
docker pause "$P-srv" >/dev/null
wait_log "$P-TA" "Connection lost" 60 && wait_log "$P-E" "Connection lost" 60
check $? "TA and E notice the server is gone"
docker exec "$P-TA" ping -c 5 -W 2 "$TB" >/dev/null
check $? "TA still reaches TB directly (TAP)"
docker exec "$P-E" ping -c 5 -W 2 "$A" >/dev/null
check $? "E still reaches A directly (TUN)"
before=$(docker logs "$P-TA" 2>&1 | grep -c "Connection to server .* established")
docker unpause "$P-srv" >/dev/null
for i in $(seq 1 60); do
    [ "$(docker logs "$P-TA" 2>&1 | grep -c "Connection to server .* established")" -gt "$before" ] && break
    sleep 1
done
[ "$(docker logs "$P-TA" 2>&1 | grep -c "Connection to server .* established")" -gt "$before" ] &&
    docker exec "$P-TA" ping -c 3 -W 2 "$TB" >/dev/null
check $? "TA connects again when the server is back, TB still reachable"

echo
if [ "$FAILS" = 0 ]; then
    echo "test-e2e-mesh: all checks passed"
else
    echo "test-e2e-mesh: $FAILS check(s) FAILED"
    exit 1
fi
