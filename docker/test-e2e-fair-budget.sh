#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-e2e-fair-budget.sh — regression test for issue #78
#
# Every client knows the server's public key, so it can send INITs that
# pass mac1 but open with no key. Before #78 such a flood used up the
# shared budgets (200 handshakes and the key trials a second) and nobody
# else could register. The server has 3000 per-client keys; the clients
# below use the last ones, so each of their handshakes tries them all.
#
# 1. One address floods bogus INITs: a client from another /24 and one
#    from the flooding address's own /24 still register within seconds.
# 2. Eight /24s flood together, using up what unknown sources may take: a
#    live client keeps rekeying from its address (its own key is tried
#    first) and carries traffic.
#
# Usage: ./test-e2e-fair-budget.sh
#   CAMEX_BINARY=path/to/camex  binary to test (default: ../camex)
#   KEEP=1                      keep containers for debugging
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/camex}"
IMAGE="camex:e2e-fair"
P="camex-fair-$$"
NET_NAME="$P-net"
SUBNET="${FAIR_SUBNET:-10.231}"     # a /16 of its own: sources in several /24s
SRV_IP="$SUBNET.0.2"
BUILD_CTX="$(mktemp -d)"
KEYS=3000
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
build_v2tool "$BUILD_CTX" || { echo "cannot build tests/v2tool"; exit 1; }
cat > "$BUILD_CTX/Dockerfile" <<'DOCKERFILE'
FROM ubuntu:24.04
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
        iputils-ping iproute2 >/dev/null && rm -rf /var/lib/apt/lists/*
COPY camex /usr/local/bin/camex
COPY server.key /etc/camex/server.key
COPY v2tool /usr/local/bin/v2tool
COPY camex.conf /etc/camex.conf
RUN printf '#!/bin/sh\nmkdir -p /dev/net\n[ -e /dev/net/tun ] || mknod /dev/net/tun c 10 200\nexec /usr/local/bin/camex "$@"\n' \
        > /usr/local/bin/camex-run && chmod +x /usr/local/bin/camex-run
ENTRYPOINT ["/usr/local/bin/camex-run"]
DOCKERFILE
# 3000 clients with keys of their own, hex: (no Argon2id at startup); the
# clients of this test are the last three
key() { printf 'hex:%064x' "$1"; }
{
    echo "[server]"
    for i in $(seq 1 $((KEYS - 3))); do
        printf '[client K%04d]\npsk=%s\n' "$i" "$(key "$i")"
    done
    printf '[client FAR]\npsk=%s\n' "$(key 90001)"
    printf '[client NEAR]\npsk=%s\n' "$(key 90002)"
    printf '[client LIVE]\npsk=%s\n' "$(key 90003)"
} > "$BUILD_CTX/camex.conf"
docker build -q -t "$IMAGE" "$BUILD_CTX" >/dev/null
docker network create --subnet "$SUBNET.0.0/16" "$NET_NAME" >/dev/null
set +e   # checks below report failures themselves

docker run -d --name "$P-srv" --network "$NET_NAME" --ip "$SRV_IP" \
    --cap-add NET_ADMIN --cap-add MKNOD \
    "$IMAGE" --mode server --port 5800 --local-cidr 10.200.0.1/24 \
    $SERVER_KEY_ARGS --config /etc/camex.conf >/dev/null
sleep 2

client() {  # client <name> <ip> [env...]: an auto-mode client with its key
    local name="$1" ip="$2" n
    shift 2
    case "$name" in FAR) n=90001 ;; NEAR) n=90002 ;; LIVE) n=90003 ;; esac
    docker run -d --name "$P-$name" --network "$NET_NAME" --ip "$ip" \
        --cap-add NET_ADMIN --cap-add MKNOD "$@" \
        "$IMAGE" --mode client --auto --name "$name" --server-host "$SRV_IP" \
        --port 5800 --server-pubkey "$SERVER_PUBKEY" --psk "$(key $n)" >/dev/null
}

flood() {   # flood <name> <ip>: bogus INITs (valid mac1, no key opens them;
            # a hex: key, so the tool runs no Argon2id per batch). 50 a second
            # from each: twice an address's share, and eight of them over the
            # unknown sources' share, without filling the server's socket
            # buffer (a slow CI host then drops any packet, the live client's
            # too, which no budget can help)
    docker run -d --name "$P-$1" --network "$NET_NAME" --ip "$2" -e NOWAIT=1 -e PACE_MS=20 \
        --entrypoint /bin/sh "$IMAGE" -c \
        "while :; do /usr/local/bin/v2tool $SRV_IP 5800 $SERVER_PUBKEY $(key 99999) udp \
            \$(seq -f auto:F%g 1 100) >/dev/null; done" >/dev/null
}

wait_log() {  # wait_log <container> <text> <seconds>
    local _
    for _ in $(seq 1 "$3"); do
        docker logs "$P-$1" 2>&1 | grep -q "$2" && return 0
        sleep 1
    done
    return 1
}

echo "== 1. one address floods: clients from other addresses still register"
flood fl1 "$SUBNET.1.10"
sleep 3
client FAR "$SUBNET.2.10"
client NEAR "$SUBNET.1.20"
wait_log FAR "Received config from server" 20
check $? "a client from another /24 registers during the flood"
wait_log NEAR "Received config from server" 20
check $? "a client from the flooding address's /24 registers during the flood"
docker logs "$P-srv" 2>&1 | grep -q "its address or its /24 used its share"
check $? "the server drops the flood over the source's share"
docker rm -f "$P-fl1" "$P-FAR" "$P-NEAR" >/dev/null 2>&1

echo "== 2. eight /24s flood: a live client keeps rekeying"
client LIVE "$SUBNET.3.10" -e CAMEX_REKEY_AFTER=5
wait_log LIVE "Received config from server" 20
check $? "the live client registers before the flood"
sleep 2
for n in $(seq 1 8); do
    flood "fl2-$n" "$SUBNET.$((10 + n)).10"
done
sleep 3
before=$(docker logs "$P-LIVE" 2>&1 | grep -c "Session keys renewed")
sleep 30
after=$(docker logs "$P-LIVE" 2>&1 | grep -c "Session keys renewed")
[ "$after" -ge $((before + 2)) ]
check $? "the live client renews its keys during the flood ($before -> $after)"
docker logs "$P-srv" 2>&1 | grep -q "unknown sources used their share"
check $? "unknown sources hit their share of the budget"
LIVE_IP=$(docker exec "$P-LIVE" ip -4 -o addr show 2>/dev/null |
          awk '$2 != "lo" && $4 ~ /^10\.200\./ { sub("/.*", "", $4); print $4 }')
[ -n "$LIVE_IP" ] && docker exec "$P-srv" ping -c 3 -W 2 "$LIVE_IP" >/dev/null 2>&1
check $? "traffic to the live client works during the flood (${LIVE_IP:-no address})"

if [ "$FAILS" -ne 0 ]; then
    echo "FAIL: $FAILS check(s) failed"
    exit 1
fi
echo "PASS: a flood of bogus handshakes does not keep other sources or live clients out"
