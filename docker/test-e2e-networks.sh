#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-e2e-networks.sh — named networks: a client joins with the server's
# address and the network's name (its ID is given or its MAC); its address
# comes from the network and the ID, the same after restarts
#
#  1. First contact: no --server-pubkey, no --psk: the client asks for the
#     server's key, keeps it in the known servers file, gets an address in
#     the network's pool and reaches the network's gateway
#  2. The kept key is used at the next start
#  3. Client restart, server restart: the same address
#  4. A second client of the network reaches the first one
#  5. Another network's client cannot reach them
#  6. An unknown network gets nothing; the name is never logged
#  7. Two client IDs whose hashes meet: the second gets +1; the first one
#     coming back after a kill (its old session still live) gets its own
#     address back, never +1
#  8. The file is read again when it changes: a new network works without
#     a signal; a broken file keeps the old configuration
#  9. allow_new_networks=yes: any name makes a new network
# 10. camex --top shows the clients and their rates
# 11. A server with a new key: the client says how to forget the old one
#
# Usage: ./test-e2e-networks.sh
#   CAMEX_BINARY=path/to/camex  binary to test (default: ../camex)
#   KEEP=1                      keep containers for debugging
#

set -e
export LC_ALL=C

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/camex}"
IMAGE="camex:e2e-networks"
P="camex-nets-$$"
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

cp "$CAMEX_BINARY" "$BUILD_CTX/camex"
. "$SCRIPT_DIR/lib-keys.sh"
gen_server_key "$BUILD_CTX" "$CAMEX_BINARY" || { echo "cannot create a server key"; exit 1; }
cat > "$BUILD_CTX/Dockerfile" <<'DOCKERFILE'
FROM ubuntu:24.04
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
        iputils-ping iproute2 python3-minimal >/dev/null && rm -rf /var/lib/apt/lists/*
COPY camex /usr/local/bin/camex
COPY server.key /etc/camex/server.key
RUN printf '#!/bin/sh\nmkdir -p /dev/net\n[ -e /dev/net/tun ] || mknod /dev/net/tun c 10 200\nexec /usr/local/bin/camex "$@"\n' \
        > /usr/local/bin/camex-run && chmod +x /usr/local/bin/camex-run
ENTRYPOINT ["/usr/local/bin/camex-run"]
DOCKERFILE
docker build -q -t "$IMAGE" "$BUILD_CTX" >/dev/null 2>&1
docker network create "$NET_NAME" >/dev/null
set +e   # checks below report failures themselves

cat > "$BUILD_CTX/camex.conf" <<'EOF'
[server]
local_cidr = 10.200.0.1/24

[network field cameras]
weak_name = yes
label = cams

[network lab]
weak_name = yes
pool = 10.77.0.0/24
EOF

server() {  # server <name> <config file> [camex args...]
    local name="$1" conf="$2"; shift 2
    docker create --name "$P-$name" --network "$NET_NAME" --cap-add NET_ADMIN \
        --cap-add MKNOD "$IMAGE" --mode server --port 5800 \
        $SERVER_KEY_ARGS --config /etc/camex.conf "$@" >/dev/null
    docker cp "$conf" "$P-$name:/etc/camex.conf" >/dev/null
    docker start "$P-$name" >/dev/null
}

client() {  # client <name> <server> <network> [camex args...]: three inputs
    local name="$1" srv="$2" net="$3"; shift 3
    docker run -d --name "$P-$name" --network "$NET_NAME" --cap-add NET_ADMIN \
        --cap-add MKNOD "$IMAGE" --server-host "$P-$srv" --network "$net" \
        "$@" >/dev/null
}

addr_of() { # addr_of <container>: its camex0 address/prefix, after up to 15 s
    local i a
    for i in $(seq 1 30); do
        a=$(docker exec "$1" ip -4 -o addr show 2>/dev/null |
            awk '$2 ~ /^camex|^tun/ { print $4; exit }')
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

# The address a network gives an ID before probing (netw.c), and an ID of
# the same first choice: the collision test needs two of them
cat > "$BUILD_CTX/netaddr.py" <<'EOF'
import hashlib, sys
def netid(name):
    return hashlib.blake2b(b"camex network v1" + name.encode(), digest_size=32).digest()
def base(name, cid, first, hosts):
    h = hashlib.blake2b(b"camex network address v1" + netid(name) + cid.lower().encode(),
                        digest_size=8).digest()
    return first + int.from_bytes(h, "big") % hosts
name, first, hosts = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
want = base(name, "COLL-A", first, hosts)
for i in range(100000):
    cid = "COLL-B%d" % i
    if base(name, cid, first, hosts) == want:
        print(want, cid)
        break
EOF

echo "== 1. first contact: server address and network name only"
server srv "$BUILD_CTX/camex.conf"
sleep 1
client a srv "field cameras" --name CAM-A
A=$(addr_of "$P-a")
check $? "client A has an address: $A"
case "$A" in 10.*/16) r=0 ;; *) r=1 ;; esac
check $r "A's address is in a derived /16 of 10/8 ($A)"
docker exec "$P-a" grep -q "^$P-srv:5800 $SERVER_PUBKEY pake\$" /etc/camex/known_servers
check $? "the server's key is kept in /etc/camex/known_servers, marked pake"
docker logs "$P-a" 2>&1 | grep -q "proved it knows the network (CPace)"
check $? "the server proved it knows the network's name (CPace)"
GW=$(echo "$A" | awk -F'[./]' '{ print $1"."$2".0.1" }')
docker exec "$P-a" ping -c 2 -W 2 "$GW" >/dev/null
check $? "A reaches its network's gateway $GW"
docker logs "$P-srv" 2>&1 | grep -q "field cameras"
r=$?; [ $r != 0 ]; check $? "the server never logs the network's name"
docker exec "$P-a" ps -o args= -C camex 2>/dev/null | grep -q "field cameras"
r=$?; [ $r != 0 ]; check $? "the network's name is not in the client's command line"

echo "== 2. the next start"
docker restart "$P-a" >/dev/null
A2=$(addr_of "$P-a")
[ "$A2" = "$A" ]; check $? "client restart: the same address ($A2)"
[ "$(docker logs "$P-a" 2>&1 | grep -c "proved it knows the network")" = 1 ]
check $? "the same key: kept as it was, nothing new said"

echo "== 3. server restart: the same address"
docker restart "$P-srv" >/dev/null
sleep 1
docker exec "$P-a" sh -c 'pkill -x camex' 2>/dev/null
docker restart "$P-a" >/dev/null
A3=$(addr_of "$P-a")
[ "$A3" = "$A" ]; check $? "after a server restart: the same address ($A3)"

echo "== 4. two clients of one network"
client b srv "field cameras" --name CAM-B
B=$(addr_of "$P-b")
check $? "client B has an address: $B"
[ "${B%.*.*}" = "${A%.*.*}" ] && [ "$B" != "$A" ]
check $? "B is in A's network, with another address"
docker exec "$P-b" ping -c 2 -W 2 "${A%/*}" >/dev/null
check $? "B reaches A through the server"

echo "== 5. another network"
client c srv lab --name LAB-C
C=$(addr_of "$P-c")
case "$C" in 10.77.0.*/24) r=0 ;; *) r=1 ;; esac
check $r "the lab client has its pool= address ($C)"
# A route into the other network's pool: the server must still drop it
docker exec "$P-c" sh -c "ip route add ${A%.*.*}.0.0/16 dev \$(ip -o -4 addr show | awk '\$4 ~ /^10.77/ { print \$2 }')"
docker exec "$P-c" ping -c 2 -W 1 "${A%/*}" >/dev/null 2>&1
r=$?; [ $r != 0 ]; check $? "lab cannot reach field cameras"
docker exec "$P-srv" camex --status --json | python3 -c '
import json, sys
s = json.load(sys.stdin)["servers"][0]
c = [c for c in s["clients"] if c["id"] == "LAB-C"][0]
assert c["isolation_drops"] >= 1
' 2>/dev/null
check $? "the server drops it (isolation_drops of LAB-C)"

echo "== 6. an unknown network"
client d srv "no such network" --name CAM-D
sleep 4
docker logs "$P-d" 2>&1 | grep -q "does not know this network"
check $? "the client finds the server does not know the network (CPace)"
docker logs "$P-srv" 2>&1 | grep -q "no such network"
r=$?; [ $r != 0 ]; check $? "the unknown name is not logged"
docker rm -f "$P-d" >/dev/null

echo "== 7. two IDs with one first choice, and a killed client coming back"
# lab: 10.77.0.2 .. .254, 253 client addresses
read -r BASE COLL <<<"$(python3 "$BUILD_CTX/netaddr.py" lab $((0x0A4D0002)) 253)"
BASE_IP=$(python3 -c "import ipaddress; print(ipaddress.ip_address($BASE))")
NEXT_IP=$(python3 -c "import ipaddress; print(ipaddress.ip_address($BASE + 1))")
client x srv lab --name COLL-A
X=$(addr_of "$P-x")
[ "$X" = "$BASE_IP/24" ]; check $? "COLL-A gets the hash's address ($X, expected $BASE_IP)"
client y srv lab --name "$COLL"
Y=$(addr_of "$P-y")
[ "$Y" = "$NEXT_IP/24" ]; check $? "$COLL (same first choice) gets +1 ($Y, expected $NEXT_IP)"
wait_log "$P-srv" "Network net-[0-9a-f]{8}: $BASE_IP of client $COLL is taken by client COLL-A, it gets $NEXT_IP" 2
check $? "the server says why"
# Killed: its session stays live on the server; it comes back at once
docker kill -s KILL "$P-x" >/dev/null
docker start "$P-x" >/dev/null
X2=$(addr_of "$P-x")
[ "$X2" = "$BASE_IP/24" ]; check $? "COLL-A killed and back at once: its own address, not +1 ($X2)"
docker restart "$P-y" >/dev/null
Y2=$(addr_of "$P-y")
[ "$Y2" = "$NEXT_IP/24" ]; check $? "$COLL restarted: still +1 ($Y2)"

echo "== 7b. a client that is gone keeps its address"
# CAMEX_CLIENT_TIMEOUT=5: silent clients expire after 5 s (a test setting)
docker create --name "$P-srv2" --network "$NET_NAME" --cap-add NET_ADMIN \
    --cap-add MKNOD -e CAMEX_CLIENT_TIMEOUT=5 "$IMAGE" --mode server \
    --port 5800 $SERVER_KEY_ARGS --config /etc/camex.conf >/dev/null
docker cp "$BUILD_CTX/camex.conf" "$P-srv2:/etc/camex.conf" >/dev/null
docker start "$P-srv2" >/dev/null
sleep 1
client x2 srv2 lab --name COLL-A --keepalive 1
X=$(addr_of "$P-x2")
[ "$X" = "$BASE_IP/24" ]; check $? "COLL-A gets the hash's address ($X)"
docker stop -t 1 "$P-x2" >/dev/null
wait_log "$P-srv2" "Expired client $BASE_IP\$" 15
check $? "COLL-A expires"
client y2 srv2 lab --name "$COLL" --keepalive 1
Y=$(addr_of "$P-y2")
[ "$Y" = "$NEXT_IP/24" ]; check $? "$COLL does not take the address of COLL-A, which is gone ($Y)"
docker start "$P-x2" >/dev/null
X=$(addr_of "$P-x2")
[ "$X" = "$BASE_IP/24" ]; check $? "COLL-A comes back to its address ($X)"
docker rm -f "$P-x2" "$P-y2" "$P-srv2" >/dev/null

echo "== 8. the file changes: read again without a signal"
cp "$BUILD_CTX/camex.conf" "$BUILD_CTX/new.conf"
printf '\n[network annex]\nweak_name = yes\npool = 10.88.0.0/24\n' >> "$BUILD_CTX/new.conf"
docker cp "$BUILD_CTX/new.conf" "$P-srv:/etc/camex.conf" >/dev/null
wait_log "$P-srv" "Config file changed" 6
check $? "the server notices the changed file"
client e srv annex --name ANNEX-E
E=$(addr_of "$P-e")
case "$E" in 10.88.0.*/24) r=0 ;; *) r=1 ;; esac
check $r "a client of the new network has an address ($E)"
printf '[network broken\n' > "$BUILD_CTX/bad.conf"
docker cp "$BUILD_CTX/bad.conf" "$P-srv:/etc/camex.conf" >/dev/null
wait_log "$P-srv" "Keeping previous server configuration" 6
check $? "a broken file keeps the configuration"
docker exec "$P-e" ping -c 1 -W 2 10.88.0.1 >/dev/null
check $? "the new network still works"
docker cp "$BUILD_CTX/new.conf" "$P-srv:/etc/camex.conf" >/dev/null   # good again

echo "== 9. allow_new_networks=yes"
printf '[server]\nlocal_cidr = 10.200.0.1/24\nallow_new_networks = yes\n' > "$BUILD_CTX/open.conf"
server open "$BUILD_CTX/open.conf"
sleep 1
client f open "anything-$$" --name ANY-F
F=$(addr_of "$P-f")
check $? "a client of a name the server never heard of gets an address ($F)"
wait_log "$P-open" "New network net-[0-9a-f]{8} \\(pool 10\\.[0-9]+\\.0\\.0/16\\), created by client ANY-F" 2
check $? "the server makes the new network"
docker logs "$P-f" 2>&1 | grep -q "does not know the network but takes new ones: joining by the network's ID"
check $? "the client joins a new network the old way, and says so"

echo "== 10. camex --top"
docker exec "$P-srv" sh -c 'timeout 3 camex --top --interval 1 </dev/null' > "$BUILD_CTX/top.txt" 2>&1
grep -q "IN Mbit/s" "$BUILD_CTX/top.txt" && grep -q "CAM-A" "$BUILD_CTX/top.txt" &&
    grep -q "^NETWORK " "$BUILD_CTX/top.txt"
check $? "--top lists clients with rates and the networks"
docker exec "$P-srv" camex --status --json | python3 -c '
import json, sys
s = json.load(sys.stdin)["servers"][0]
assert any(n["label"] == "cams" for n in s["networks"])
c = [c for c in s["clients"] if c["id"] == "CAM-A"][0]
assert c["network"] == "cams" and "rx_rate" in c
' 2>/dev/null
check $? "--status --json has the networks, each client's network and rates"

echo "== 11. the server gets a new key; an impostor"
KEYS=$("$CAMEX_BINARY" --genkey)
printf '%s\n' "$KEYS" | sed -n 's/^server_key=/server_key=/p' > "$BUILD_CTX/other.key"
NEWPUB=$(printf '%s\n' "$KEYS" | sed -n 's/^public_key=//p')
chmod 600 "$BUILD_CTX/other.key"
docker cp "$BUILD_CTX/other.key" "$P-srv:/etc/camex/server.key" >/dev/null
docker restart "$P-srv" >/dev/null
docker exec "$P-a" sh -c 'pkill -x camex' 2>/dev/null
docker restart "$P-a" >/dev/null
A4=$(addr_of "$P-a")
[ "$A4" = "$A" ]; check $? "with the server's new key: back, the same address ($A4)"
docker exec "$P-a" grep -q "^$P-srv:5800 $NEWPUB pake\$" /etc/camex/known_servers
check $? "the new key, proven by the name, replaced the old one"
# An impostor at the server's name: its own key, takes any network
docker rm -f "$P-srv" >/dev/null
printf '[server]\nlocal_cidr = 10.200.0.1/24\nallow_new_networks = yes\n' > "$BUILD_CTX/imp.conf"
"$CAMEX_BINARY" --genkey | sed -n 's/^server_key=/server_key=/p' > "$BUILD_CTX/imp.key"
chmod 600 "$BUILD_CTX/imp.key"
docker create --name "$P-srv" --network "$NET_NAME" --cap-add NET_ADMIN \
    --cap-add MKNOD "$IMAGE" --mode server --port 5800 $SERVER_KEY_ARGS \
    --config /etc/camex.conf >/dev/null
docker cp "$BUILD_CTX/imp.conf" "$P-srv:/etc/camex.conf" >/dev/null
docker cp "$BUILD_CTX/imp.key" "$P-srv:/etc/camex/server.key" >/dev/null
docker start "$P-srv" >/dev/null
docker restart "$P-a" >/dev/null
wait_log "$P-a" "proved it knows this network before: not trusted" 20
check $? "the client does not take an impostor that claims to be open"
docker logs "$P-srv" 2>&1 | grep -q "New network"
r=$?; [ $r != 0 ]; check $? "the impostor learned no network"

echo "== 12. CPace limits: a guesser is limited and banned, a real client is not"
docker rm -f "$P-srv" "$P-a" >/dev/null 2>&1
GUARD_NET="guard network of 24 chars"
printf '[server]\nlocal_cidr = 10.200.0.1/24\ncpace_per_ip = 5\ncpace_ban_after = 8\ncpace_ban_time = 60\n\n[network %s]\nlabel = guarded\n' \
    "$GUARD_NET" > "$BUILD_CTX/guard.conf"
server srv "$BUILD_CTX/guard.conf"
sleep 1
# PAKE1 with a random point: the same to the server as a wrong name (no PAKE3
# follows); 10 of them 0.3 s apart, counting the PAKE2 answers
cat > "$BUILD_CTX/guess.py" <<'PY'
import os, socket, sys, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(0.25)
answers = 0
for _ in range(10):
    pkt = b"CXv2" + bytes([8]) + os.urandom(16) + os.urandom(32)
    s.sendto(pkt + bytes(1200 - len(pkt)), (sys.argv[1], 5800))
    try:
        if s.recv(4096)[4] == 9:
            answers += 1
    except socket.timeout:
        pass
    time.sleep(0.3)
print(answers)
PY
docker create --name "$P-guess" --network "$NET_NAME" --entrypoint python3 "$IMAGE" /guess.py "$P-srv" >/dev/null
docker cp "$BUILD_CTX/guess.py" "$P-guess:/guess.py" >/dev/null
answers=$(docker start -a "$P-guess" 2>/dev/null | tail -1)
[ "$answers" = 5 ]
check $? "the guesser got 5 answers of 10 (cpace_per_ip = 5), got ${answers:-none}"
wait_log "$P-srv" "CPace: 8 attempts in a row from" 5
check $? "the server bans it after 8 attempts in a row and says so"
docker exec "$P-srv" camex --status --json 2>/dev/null |
    python3 -c "import json,sys; c=json.load(sys.stdin)['servers'][0]['cpace']; sys.exit(0 if c['bans']==1 and c['banned_now']==1 and c['refused']==5 else 1)"
check $? "--status --json counts the attempts, the refusals and the ban"
client a srv "$GUARD_NET" --name CAM-G
addr_of "$P-a" >/dev/null
check $? "a real client from another address joins all the same"
docker rm -f "$P-guess" >/dev/null 2>&1

echo
if [ "$FAILS" = 0 ]; then
    echo "test-e2e-networks: all checks passed"
else
    echo "test-e2e-networks: $FAILS check(s) FAILED"
    exit 1
fi
