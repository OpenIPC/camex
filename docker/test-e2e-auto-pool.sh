#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-e2e-auto-pool.sh — regression test for issue #15
#
# 1. Server without a config file (auto pool 10.200.0.0/24): 256 auto-mode
#    REGISTERs with different client IDs, each from its own socket. The
#    pool must hand out only .2-.254, never the same address twice, and
#    then report that it is full. Before the fix it handed out .255 and
#    then wrapped to .2 while that address was still in use.
#    Failed registrations after that must not free addresses by evicting
#    live clients.
# 2. Server with allow_unknown_clients=no: a listed client ID gets its
#    configuration, an unlisted one gets nothing.
# 3. A reload that switches to allow_unknown_clients=no ends the sessions
#    of unlisted auto clients.
#
# Usage: ./test-e2e-auto-pool.sh
#   CAMEX_BINARY=path/to/camex  binary to test (default: ../camex)
#   KEEP=1                      keep containers for debugging
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/camex}"
IMAGE="camex:e2e-pool"
P="camex-pool-$$"
NET_NAME="$P-net"
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
        python3-minimal >/dev/null && rm -rf /var/lib/apt/lists/*
COPY camex /usr/local/bin/camex
COPY register.py /register.py
COPY camex.conf /etc/camex.conf
RUN printf '#!/bin/sh\nmkdir -p /dev/net\n[ -e /dev/net/tun ] || mknod /dev/net/tun c 10 200\nexec /usr/local/bin/camex "$@"\n' \
        > /usr/local/bin/camex-run && chmod +x /usr/local/bin/camex-run
ENTRYPOINT ["/usr/local/bin/camex-run"]
EOF
# register.py <host> <id>...: send a plaintext auto-mode REGISTER for each
# client ID from its own socket and print "<id> <LOCAL_CIDR or ->".
cat > "$BUILD_CTX/register.py" <<'EOF'
import socket, sys
host, ids = sys.argv[1], sys.argv[2:]
socks = []
for cid in ids:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(1.0)
    s.sendto(("CX2P REGISTER VER=1 MODE=AUTO CLIENT_ID=%s" % cid).encode(),
             (host, 5800))
    cidr = "-"
    try:
        for tok in s.recv(2048).decode(errors="replace").split():
            if tok.startswith("LOCAL_CIDR="):
                cidr = tok.split("=", 1)[1]
    except OSError:
        pass
    print(cid, cidr, flush=True)
    socks.append(s)   # keep the source port: one client per socket
EOF
cat > "$BUILD_CTX/camex.conf" <<'EOF'
[server]
allow_unknown_clients=no
[client KNOWN]
local_cidr=10.201.0.10/24
gateway_ip=10.201.0.1
EOF
docker build -q -t "$IMAGE" "$BUILD_CTX" >/dev/null
docker network create "$NET_NAME" >/dev/null
set +e   # checks below report failures themselves

atk() {     # atk <server> <id>...: register from an attacker container
    docker run --rm --network "$NET_NAME" --entrypoint python3 "$IMAGE" \
        /register.py "$@"
}

echo "== 1. auto pool without a config file: 256 client IDs"
docker run -d --name "$P-srv1" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    "$IMAGE" --mode server --port 5800 --local-cidr 10.200.0.1/24 >/dev/null
sleep 1
atk "$P-srv1" $(seq -f "CAM%03g" 1 256) > "$BUILD_CTX/pool.txt"
assigned=$(awk '$2 != "-"' "$BUILD_CTX/pool.txt" | wc -l)
echo "   addresses handed out: $assigned"
[ "$assigned" -gt 0 ]; check $? "the pool hands out addresses"
bad=$(awk '$2 != "-" { split($2, a, "[./]"); if (a[4] < 2 || a[4] > 254) print $2 }' "$BUILD_CTX/pool.txt" | sort -u | tr '\n' ' ')
[ -z "$bad" ]; check $? "only .2-.254 are handed out (no network, gateway or broadcast address)${bad:+: got $bad}"
dups=$(awk '$2 != "-" { print $2 }' "$BUILD_CTX/pool.txt" | sort | uniq -d | wc -l)
[ "$dups" = 0 ]; check $? "no address is handed out twice (duplicates: $dups)"
[ "$assigned" = 253 ]; check $? "the whole pool .2-.254 is used (253 addresses)"
docker logs "$P-srv1" 2>&1 | grep -q "Auto pool .* is full"
check $? "the server reports that the pool is full"
atk "$P-srv1" $(seq -f "LATE%02g" 1 10) > "$BUILD_CTX/late.txt"
late=$(awk '$2 != "-"' "$BUILD_CTX/late.txt" | wc -l)
[ "$late" = 0 ]; check $? "failed registrations do not evict live clients to free addresses (got $late)"

echo "== 2. allow_unknown_clients=no"
docker run -d --name "$P-srv2" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    "$IMAGE" --mode server --port 5800 --local-cidr 10.201.0.1/24 \
    --config /etc/camex.conf >/dev/null
sleep 1
atk "$P-srv2" KNOWN STRANGER > "$BUILD_CTX/known.txt"
grep -q "^KNOWN 10.201.0.10/24$" "$BUILD_CTX/known.txt"
check $? "a listed client ID gets its configuration"
grep -q "^STRANGER -$" "$BUILD_CTX/known.txt"
check $? "an unlisted client ID gets nothing"

echo "== 3. reload to allow_unknown_clients=no ends unknown clients' sessions"
sed 's/allow_unknown_clients=no/allow_unknown_clients=yes/' "$BUILD_CTX/camex.conf" > "$BUILD_CTX/yes.conf"
docker create --name "$P-srv3" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    "$IMAGE" --mode server --port 5800 --local-cidr 10.201.0.1/24 \
    --config /etc/camex.conf >/dev/null
docker cp "$BUILD_CTX/yes.conf" "$P-srv3:/etc/camex.conf" >/dev/null
docker start "$P-srv3" >/dev/null
sleep 1
atk "$P-srv3" GUEST > "$BUILD_CTX/guest.txt"
! grep -q "^GUEST -$" "$BUILD_CTX/guest.txt"
check $? "with allow_unknown_clients=yes an unlisted ID gets an address"
docker cp "$BUILD_CTX/camex.conf" "$P-srv3:/etc/camex.conf" >/dev/null
docker kill -s HUP "$P-srv3" >/dev/null
sleep 2
docker logs "$P-srv3" 2>&1 | grep -q "Ended session of client GUEST"
check $? "after the reload to \"no\" the unlisted client's session ends"

if [ "$FAILS" != 0 ]; then
    echo "--- server 1 log (tail) ---"
    docker logs "$P-srv1" 2>&1 | tail -5
    echo "--- server 2 log (tail) ---"
    docker logs "$P-srv2" 2>&1 | tail -5
    echo "FAIL: $FAILS check(s) failed"
    exit 2
fi
echo "PASS: the auto pool hands out unique .2-.254 addresses; unknown IDs can be refused"
