#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-e2e-register-replay.sh — regression test for issue #7 (protocol v2)
#
# An encrypted manual client registers and pings the server while its
# packets are captured. An attacker then replays the captured packets
# from its own address:
#   1. the handshake (INIT) of the live session, then the captured DATA
#      (pings):
#      the session must not move to the attacker, the pings must not
#      reach the server again;
#   2. after the client reconnected (new session): the old session's
#      INIT and DATA must be rejected as well;
#   3. more old INITs within 30 s: no new line, the next one counts them.
# The server's line says why (#135): how much older the timestamp is than
# the latest accepted for the client, and its last address.
# After each attack the client must still reach the server.
# In protocol v2 a replayed INIT carries an old timestamp, and replayed
# DATA fails the replay window or names a session that no longer exists.
#
# Usage: ./test-e2e-register-replay.sh
#   CAMEX_BINARY=path/to/camex  binary to test (default: ../camex)
#   KEEP=1                      keep containers for debugging
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/camex}"
IMAGE="camex:e2e-replay"
P="camex-replay-$$"
NET_NAME="$P-net"
SRV="$P-srv"
CLI="$P-cli"
CAP="$P-cap"
ATK="$P-atk"
BUILD_CTX="$(mktemp -d)"
PSK="replay-test"
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
cat > "$BUILD_CTX/Dockerfile" <<'EOF'
FROM ubuntu:24.04
# New clients right after the server starts: no start hold (#81)
ENV CAMEX_POOL_HOLD=0
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
        iputils-ping python3-minimal tcpdump >/dev/null && rm -rf /var/lib/apt/lists/*
COPY camex /usr/local/bin/camex
COPY server.key /etc/camex/server.key
RUN printf '#!/bin/sh\nmkdir -p /dev/net\n[ -e /dev/net/tun ] || mknod /dev/net/tun c 10 200\nexec /usr/local/bin/camex "$@"\n' \
        > /usr/local/bin/camex-run && chmod +x /usr/local/bin/camex-run
ENTRYPOINT ["/usr/local/bin/camex-run"]
EOF
# Sends hex payloads from stdin to <host> 5800/udp.
cat > "$BUILD_CTX/send.py" <<'EOF'
import socket, sys, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
n = 0
for line in sys.stdin:
    s.sendto(bytes.fromhex(line.split()[1]), (sys.argv[1], 5800))
    n += 1
    time.sleep(0.05)
print(n)
EOF
docker build -q -t "$IMAGE" "$BUILD_CTX" >/dev/null
docker network create "$NET_NAME" >/dev/null
set +e   # checks below report failures themselves

icmp_in() {  # ICMP echo requests received by a container
    docker exec "$1" awk '/^Icmp:/ { if (n++) print $10 }' /proc/net/snmp
}
still_up() {
    docker exec "$CLI" ping -c2 -W2 10.200.0.1 >/dev/null 2>&1
    check $? "the client still reaches the server"
}
start_client() {
    docker run -d --name "$CLI" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
        "$IMAGE" --mode client --local-cidr 10.200.0.2/24 --gateway-ip 10.200.0.1 \
        --server-host "$SRV" --port 5800 --server-pubkey "$SERVER_PUBKEY" --psk "$PSK" >/dev/null
}
wait_ping() {
    local i
    for i in $(seq 1 20); do
        docker exec "$CLI" ping -c1 -W1 10.200.0.1 >/dev/null 2>&1 && return 0
        sleep 1
    done
    return 1
}
# replay <file>...: send the captured payloads, in order, from one socket;
# checks that all of them were sent
replay() {
    local want sent
    want=$(cat "$@" | wc -l)
    sent=$(cat "$@" | docker exec -i "$ATK" python3 /send.py "$SRV")
    [ "$sent" = "$want" ]; check $? "attacker sent $want captured packets (sent: ${sent:-none})"
}
# dropped_regs: lines of handshakes the server dropped as not newer than
# the live session's (#135: one line in 30 s per client)
dropped_regs() {
    docker logs "$SRV" 2>&1 | grep -c "Dropped handshake.*its timestamp.*the latest accepted for this client" || true
}
# wait_quiet: past the 30 s in which the server says no more of them
wait_quiet() {
    local left=$((LAST_DROP + 31 - SECONDS))
    [ "$left" -gt 0 ] && sleep "$left"
}
CLI_IP=

echo "== start server, capture, client"
docker run -d --name "$SRV" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    "$IMAGE" --mode server --port 5800 --local-cidr 10.200.0.1/24 \
    $SERVER_KEY_ARGS --psk "$PSK" >/dev/null
docker run -d --name "$CAP" --network "container:$SRV" --cap-add NET_RAW --cap-add NET_ADMIN \
    --entrypoint tcpdump "$IMAGE" -i any -U -w /tmp/c.pcap udp port 5800 >/dev/null
docker run -d --name "$ATK" --network "$NET_NAME" --entrypoint sleep "$IMAGE" infinity >/dev/null \
    || { echo "FAIL: attacker container did not start"; exit 2; }
docker cp "$BUILD_CTX/send.py" "$ATK:/send.py" >/dev/null
sleep 2
start_client
wait_ping || { echo "FAIL: client never reached the server"; exit 2; }
docker exec "$CLI" ping -q -c 5 -i 0.2 10.200.0.1 >/dev/null 2>&1
sleep 3
docker exec "$CAP" kill -INT 1 >/dev/null 2>&1; sleep 1
docker cp "$CAP:/tmp/c.pcap" "$BUILD_CTX/c.pcap" >/dev/null
# UDP payloads sent to port 5800, as lines "<type> <hex>" (tcpdump -i any
# writes Linux cooked frames: the link header size depends on the link type).
python3 - "$BUILD_CTX/c.pcap" > "$BUILD_CTX/old.txt" <<'EOF'
import struct, sys
data = open(sys.argv[1], "rb").read()
link = struct.unpack_from("<I", data, 20)[0]
hdr = {1: 14, 113: 16, 276: 20}[link]
off = 24
while off + 16 <= len(data):
    incl = struct.unpack_from("<I", data, off + 8)[0]
    pkt = data[off + 16:off + 16 + incl]
    off += 16 + incl
    ip = pkt[hdr:]
    if len(ip) < 28 or ip[0] >> 4 != 4 or ip[9] != 17:
        continue
    udp = (ip[0] & 0x0f) * 4
    if struct.unpack_from(">H", ip, udp + 2)[0] != 5800:
        continue
    payload = ip[udp + 8:]
    if len(payload) > 5 and payload[:4] == b"CXv2":
        print(payload[4], payload.hex())
EOF
regs=$(grep -c '^1 ' "$BUILD_CTX/old.txt")
datas=$(grep -c '^3 ' "$BUILD_CTX/old.txt")
echo "   captured: $regs INIT, $datas DATA"
[ "$regs" -ge 1 ] && [ "$datas" -ge 5 ]; check $? "captured the handshake and the pings"

CLI_IP=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$CLI")

echo "== 1. replay the live session's INIT and DATA from another address"
before=$(icmp_in "$SRV")
drops=$(dropped_regs)
grep '^1 ' "$BUILD_CTX/old.txt" | tail -n 1 > "$BUILD_CTX/reg-last.txt"
grep '^3 ' "$BUILD_CTX/old.txt" > "$BUILD_CTX/data.txt"
replay "$BUILD_CTX/reg-last.txt" "$BUILD_CTX/data.txt"
sleep 2
after=$(icmp_in "$SRV")
[ "$after" = "$before" ]; check $? "replayed pings do not reach the server (echo requests: $before -> $after)"
[ "$(dropped_regs)" -gt "$drops" ]; check $? "the server rejected the replayed handshake"
LAST_DROP=$SECONDS
docker logs "$SRV" 2>&1 | grep -q "Dropped handshake from .*: its timestamp is not newer than the latest accepted for this client, live, last from $CLI_IP:[0-9]*: a replay, another client with this ID, or this client's clock is behind (it is taken once the client is silent for 120 s)"
check $? "the server says why: the same timestamp as the live session from the client's address (#135)"
docker logs "$SRV" 2>&1 | grep -q "reconnected: .*$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$ATK")"
[ $? != 0 ]; check $? "the session did not move to the attacker's address"
still_up

echo "== 2. the client reconnects; replay the old session"
docker restart -t 1 "$CLI" >/dev/null
wait_ping; check $? "the client registered a new session"
sleep 6
CLI_IP=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$CLI")
wait_quiet
before=$(icmp_in "$SRV")
drops=$(dropped_regs)
grep '^1 ' "$BUILD_CTX/old.txt" > "$BUILD_CTX/regs.txt"
replay "$BUILD_CTX/regs.txt" "$BUILD_CTX/data.txt"
sleep 2
after=$(icmp_in "$SRV")
[ "$after" = "$before" ]; check $? "old session's pings do not reach the server (echo requests: $before -> $after)"
[ "$(dropped_regs)" -eq $((drops + 1)) ]; check $? "the $regs old handshakes rejected, said in one line"
LAST_DROP=$SECONDS
docker logs "$SRV" 2>&1 | grep -qE "Dropped handshake from .*: its timestamp is [0-9]+ s older than the latest accepted for this client, live, last from $CLI_IP:[0-9]+: "
check $? "the server says how much older the old session's timestamp is (#135)"
still_up

echo "== 3. more of them within 30 s: counted, said with the next line"
drops=$(dropped_regs)
replay "$BUILD_CTX/regs.txt"
sleep 1
[ "$(dropped_regs)" -eq "$drops" ]; check $? "no new line within 30 s of the last one"
wait_quiet
replay "$BUILD_CTX/reg-last.txt"
sleep 1
docker logs "$SRV" 2>&1 | tail -n 5 | grep -q "Dropped handshake.*\[$regs more since the last such line\]"
check $? "the next line says the $regs not said"
still_up

if [ "$FAILS" != 0 ]; then
    echo "--- server log (tail) ---"
    docker logs "$SRV" 2>&1 | tail -20
    echo "FAIL: $FAILS check(s) failed"
    exit 2
fi
echo "PASS: replayed handshake and DATA packets are rejected"
