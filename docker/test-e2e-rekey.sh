#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-e2e-rekey.sh — protocol v2 rekeying without loss (#17)
#
# The client renews its session keys every 5 s (CAMEX_REKEY_AFTER, a test
# setting; normally 10 min) while pings run in both directions, over UDP
# and over TCP. Every ping must get through: the server keeps sending on
# the old session until the client's first packet on the new one, and the
# old session keeps receiving for a while. Then the server restarts: the
# client gets its previous auto-pool address back.
#
# Usage: ./test-e2e-rekey.sh
#   CAMEX_BINARY=path/to/camex  binary to test (default: ../camex)
#   KEEP=1                      keep containers for debugging
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/camex}"
IMAGE="camex:e2e-rekey"
P="camex-rekey-$$"
NET_NAME="$P-net"
SRV="$P-srv"
CLI="$P-cli"
BUILD_CTX="$(mktemp -d)"
PSK="rekey-test"
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
# New clients right after the server starts: no start hold (#81)
ENV CAMEX_POOL_HOLD=0
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
        iputils-ping >/dev/null && rm -rf /var/lib/apt/lists/*
COPY camex /usr/local/bin/camex
COPY server.key /etc/camex/server.key
RUN printf '#!/bin/sh\nmkdir -p /dev/net\n[ -e /dev/net/tun ] || mknod /dev/net/tun c 10 200\nexec /usr/local/bin/camex "$@"\n' \
        > /usr/local/bin/camex-run && chmod +x /usr/local/bin/camex-run
ENTRYPOINT ["/usr/local/bin/camex-run"]
EOF
docker build -q -t "$IMAGE" "$BUILD_CTX" >/dev/null
docker network create "$NET_NAME" >/dev/null
set +e   # checks below report failures themselves

# loss <container> <target>: packets lost out of 300 (30 s at 0.1 s)
loss() {
    docker exec "$1" ping -q -c 300 -i 0.1 -W 1 "$2" 2>/dev/null |
        awk '/packets transmitted/ { print $1 - $4 }'
}

for transport in ${TRANSPORTS:-udp tcp}; do
    echo "== $transport: rekey every 5 s under traffic"
    docker rm -f "$SRV" "$CLI" >/dev/null 2>&1
    docker run -d --name "$SRV" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
        "$IMAGE" --mode server --port 5800 --transport "$transport" \
        --local-cidr 10.200.0.1/24 $SERVER_KEY_ARGS --psk "$PSK" >/dev/null
    sleep 1
    docker run -d --name "$CLI" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
        -e CAMEX_REKEY_AFTER=5 \
        "$IMAGE" --mode client --auto --name REKEY01 --transport "$transport" \
        --server-host "$SRV" --port 5800 \
        --server-pubkey "$SERVER_PUBKEY" --psk "$PSK" >/dev/null
    up=1
    for _ in $(seq 1 20); do
        docker exec "$CLI" ping -c1 -W1 10.200.0.1 >/dev/null 2>&1 && { up=0; break; }
        sleep 1
    done
    check $up "$transport: the client reaches the server"
    CLI_IP=$(docker exec "$CLI" sh -c "ip -4 -o addr show 2>/dev/null | awk '/10\.200\./ { split(\$4, a, \"/\"); print a[1] }'" 2>/dev/null)
    [ -n "$CLI_IP" ] || CLI_IP=10.200.0.2
    up_loss_file="$BUILD_CTX/up.$transport"
    ( loss "$CLI" 10.200.0.1 > "$up_loss_file" ) &
    down=$(loss "$SRV" "$CLI_IP")
    wait
    up=$(cat "$up_loss_file")
    renewals=$(docker logs "$CLI" 2>&1 | grep -c "Session keys renewed")
    [ "${up:-1}" = 0 ] && [ "${down:-1}" = 0 ]
    check $? "$transport: no ping lost in either direction (client->server: ${up:-?}, server->client: ${down:-?} of 300)"
    [ "$renewals" -ge 4 ]; check $? "$transport: the keys were renewed under traffic ($renewals times)"

    # A restarted server has an empty auto pool: the client asks for its
    # previous address and gets it back, so its tunnel stays valid
    docker restart -t 1 "$SRV" >/dev/null
    up=1
    for _ in $(seq 1 40); do
        docker exec "$SRV" ping -c1 -W1 "$CLI_IP" >/dev/null 2>&1 && { up=0; break; }
        sleep 1
    done
    check $up "$transport: after a server restart the client is back at $CLI_IP"
    ! docker logs "$CLI" 2>&1 | grep -q "different tunnel configuration"
    check $? "$transport: the client never got a different address"
done

if [ "$FAILS" != 0 ]; then
    echo "--- client log (tail) ---"
    docker logs "$CLI" 2>&1 | tail -10
    echo "--- server log (tail) ---"
    docker logs "$SRV" 2>&1 | tail -10
    echo "FAIL: $FAILS check(s) failed"
    exit 2
fi
echo "PASS: rekeying loses no packets over UDP and TCP"
