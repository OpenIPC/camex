#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-e2e-auto-pool.sh — regression test for issue #15
#
# 1. Server without a config file (auto pool 10.200.0.0/24): 256 auto-mode
#    handshakes with different client IDs, each from its own socket
#    (tests/v2tool). The
#    pool must hand out only .2-.254, never the same address twice, and
#    then report that it is full. Before the fix it handed out .255 and
#    then wrapped to .2 while that address was still in use.
#    Failed registrations after that must not free addresses by evicting
#    live clients.
# 2. Server with allow_unknown_clients=no: a listed client ID gets its
#    configuration, an unlisted one gets nothing.
# 3. A reload that switches to allow_unknown_clients=no ends the sessions
#    of unlisted auto clients.
# 4. A client that goes silent expires (CAMEX_CLIENT_TIMEOUT=5 here) and its
#    address is handed out again; while it is live, nobody else gets it.
# 5. auto_pool= and max_clients= (#75): a /16 pool serves more than 256
#    clients with unique addresses and the pool's prefix; a real client in
#    a slot above 256 carries traffic both ways; max_clients= sets the
#    slots; a /30 pool has one client address; invalid values stop the
#    server; a reload moves clients to a new pool; TCP slots stay below
#    FD_SETSIZE; a pool outside the server's tunnel network is reported;
#    slots above the memory lock limit run unlocked with a warning.
#
# Usage: ./test-e2e-auto-pool.sh
#   CAMEX_BINARY=path/to/camex  binary to test (default: ../camex)
#   KEEP=1                      keep containers for debugging
#

set -e
# Byte order for sort/uniq: in other locales "10.60.1.23/16" and
# "10.60.12.3/16" collate as equal, which hides or invents duplicates
export LC_ALL=C

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/camex}"
IMAGE="camex:e2e-pool"
P="camex-pool-$$"
NET_NAME="$P-net"
BUILD_CTX="$(mktemp -d)"
PSK="pool-test"
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
        python3-minimal iputils-ping >/dev/null && rm -rf /var/lib/apt/lists/*
COPY camex /usr/local/bin/camex
COPY server.key /etc/camex/server.key
COPY v2tool /usr/local/bin/v2tool
COPY camex.conf /etc/camex.conf
RUN printf '#!/bin/sh\nmkdir -p /dev/net\n[ -e /dev/net/tun ] || mknod /dev/net/tun c 10 200\nexec /usr/local/bin/camex "$@"\n' \
        > /usr/local/bin/camex-run && chmod +x /usr/local/bin/camex-run
ENTRYPOINT ["/usr/local/bin/camex-run"]
EOF
build_v2tool "$BUILD_CTX" || { echo "cannot build tests/v2tool"; exit 1; }
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

atk() {     # atk <server> <id>...: auto-mode handshakes, one socket each;
            # prints "<id> <LOCAL_CIDR or ->"
    local srv="$1"; shift
    docker run --rm --network "$NET_NAME" -e PACE_MS=6 --ulimit nofile=4096 \
        --entrypoint /usr/local/bin/v2tool "$IMAGE" \
        "$srv" 5800 "$SERVER_PUBKEY" "$PSK" udp $(printf 'auto:%s ' "$@") |
        sed 's/^auto://'
}

echo "== 1. auto pool without a config file: 256 client IDs"
docker run -d --name "$P-srv1" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    "$IMAGE" --mode server --port 5800 --local-cidr 10.200.0.1/24 \
    $SERVER_KEY_ARGS --psk "$PSK" >/dev/null
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
    $SERVER_KEY_ARGS --psk "$PSK" --config /etc/camex.conf >/dev/null
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
    $SERVER_KEY_ARGS --psk "$PSK" --config /etc/camex.conf >/dev/null
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

echo "== 4. an expired client's address is handed out again"
# CAMEX_CLIENT_TIMEOUT=5 (a test setting; normally 120 s) expires clients
# that are silent for 5 s; "auto:ID@IP" asks for that address
docker run -d --name "$P-srv4" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    -e CAMEX_CLIENT_TIMEOUT=5 \
    "$IMAGE" --mode server --port 5800 --local-cidr 10.200.0.1/24 \
    $SERVER_KEY_ARGS --psk "$PSK" >/dev/null
sleep 1
atk "$P-srv4" OLD > "$BUILD_CTX/old.txt"
grep -q "^OLD 10.200.0.2/24$" "$BUILD_CTX/old.txt"
check $? "the first client gets 10.200.0.2"
atk "$P-srv4" NEW@10.200.0.2 > "$BUILD_CTX/new.txt"
grep -q "^NEW@10.200.0.2 10.200.0.[0-9]*/24$" "$BUILD_CTX/new.txt" &&
    ! grep -q " 10.200.0.2/24$" "$BUILD_CTX/new.txt"
check $? "while it is live, another client asking for .2 gets another address ($(cut -d' ' -f2 "$BUILD_CTX/new.txt"))"
expired=1
for _ in $(seq 1 15); do
    docker logs "$P-srv4" 2>&1 | grep -q "Expired client 10.200.0.2$" && { expired=0; break; }
    sleep 1
done
check $expired "the silent client expires"
atk "$P-srv4" LATER@10.200.0.2 > "$BUILD_CTX/later.txt"
grep -q "^LATER@10.200.0.2 10.200.0.2/24$" "$BUILD_CTX/later.txt"
check $? "after the expiry .2 is handed out again ($(cut -d' ' -f2 "$BUILD_CTX/later.txt"))"

srv_conf() {    # srv_conf <name> <config text> [camex args...]: server with a config
    local name="$1" conf="$2"; shift 2
    printf '%b' "$conf" > "$BUILD_CTX/$name.conf"
    docker create --name "$P-$name" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
        ${SRV_CAPS:-} \
        "$IMAGE" --mode server --port 5800 $SERVER_KEY_ARGS --psk "$PSK" \
        --config /etc/camex.conf "$@" >/dev/null
    docker cp "$BUILD_CTX/$name.conf" "$P-$name:/etc/camex.conf" >/dev/null
    docker start "$P-$name" >/dev/null
}

echo "== 5. auto_pool=10.60.0.0/16: more than 256 clients (#75)"
# 65533 slots are ~84 MB: more than Docker's memory lock limit, so this
# server may lock them (IPC_LOCK); section 12 runs one without
SRV_CAPS="--cap-add IPC_LOCK" srv_conf big '[server]\nauto_pool=10.60.0.0/16\n' --local-cidr 10.60.0.1/16
sleep 1
! docker logs "$P-big" 2>&1 | grep -q "SECURITY WARNING"
check $? "with IPC_LOCK the slots are locked (no SECURITY WARNING)"
docker logs "$P-big" 2>&1 | grep -q "Client slots: 65533, auto pool 10.60.0.0/16"
check $? "without max_clients= the slots follow the pool (65533)"
! docker logs "$P-big" 2>&1 | grep -q "Auto pool .* not inside\|the server's tunnel address is"
check $? "no pool warning when the server is the pool gateway"
atk "$P-big" $(seq -f "BIG%04g" 1 600) > "$BUILD_CTX/big.txt"
assigned=$(awk '$2 != "-"' "$BUILD_CTX/big.txt" | wc -l)
[ "$assigned" = 600 ]; check $? "600 clients get addresses (got $assigned)"
dups=$(awk '$2 != "-" { print $2 }' "$BUILD_CTX/big.txt" | sort | uniq -d | wc -l)
[ "$dups" = 0 ]; check $? "no address is handed out twice (duplicates: $dups)"
bad=$(awk '$2 != "-" && ($2 !~ /^10\.60\.[0-9]+\.[0-9]+\/16$/ || $2 ~ /^10\.60\.0\.[01]\// || $2 ~ /^10\.60\.255\.255\//) { print $2 }' "$BUILD_CTX/big.txt" | head -3 | tr '\n' ' ')
[ -z "$bad" ]; check $? "addresses are 10.60.x.y/16 without the network, gateway or broadcast${bad:+: got $bad}"
# A real client registers in slot 601: its session index and address must
# still find it, both ways
docker run -d --name "$P-real" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    "$IMAGE" --mode client --auto --name REALCLIENT --server-host "$P-big" --port 5800 \
    --server-pubkey "$SERVER_PUBKEY" --psk "$PSK" >/dev/null
real_ip=""
for _ in $(seq 1 20); do
    real_ip=$(docker logs "$P-big" 2>&1 | sed -n 's|.*Auto-assigned \([0-9.]*\)/16 to client REALCLIENT.*|\1|p' | tail -1)
    [ -n "$real_ip" ] && break
    sleep 1
done
[ -n "$real_ip" ]; check $? "the real client gets an address after 600 others (${real_ip:-none})"
sleep 2
docker exec "$P-big" ping -c 3 -W 2 "$real_ip" >/dev/null 2>&1
check $? "the server reaches the client in a slot above 256 through the tunnel"
docker exec "$P-real" ping -c 3 -W 2 10.60.0.1 >/dev/null 2>&1
check $? "the client in a slot above 256 reaches the server through the tunnel"

echo "== 6. max_clients=300 with a /16 pool"
srv_conf max '[server]\nauto_pool=10.61.0.0/16\nmax_clients=300\n' --local-cidr 10.61.0.1/16
sleep 1
docker logs "$P-max" 2>&1 | grep -q "Client slots: 300, auto pool 10.61.0.0/16"
check $? "max_clients= sets the number of slots"

echo "== 7. a /30 pool has one client address"
srv_conf p30 '[server]\nauto_pool=10.70.0.0/30\n' --local-cidr 10.70.0.1/30
sleep 1
atk "$P-p30" ONE TWO > "$BUILD_CTX/p30.txt"
grep -q "^ONE 10.70.0.2/30$" "$BUILD_CTX/p30.txt"
check $? "the first client gets 10.70.0.2/30 ($(sed -n 's/^ONE //p' "$BUILD_CTX/p30.txt"))"
grep -q "^TWO -$" "$BUILD_CTX/p30.txt"
check $? "the second gets nothing"
docker logs "$P-p30" 2>&1 | grep -q "Auto pool 10.70.0.0/30 is full"
check $? "the server reports the /30 pool as full"
docker logs "$P-p30" 2>&1 | grep -q "Client slots: 256"
check $? "a small pool keeps 256 slots"

echo "== 8. invalid auto_pool= and max_clients= stop the server"
for bad_conf in 'auto_pool=10.71.0.1/24|host bits' 'auto_pool=10.71.0.0/31|prefix' \
                'auto_pool=10.71.0.0|network/prefix' 'max_clients=0|max_clients' \
                'max_clients=65535|max_clients'; do
    line=${bad_conf%%|*}; want=${bad_conf#*|}
    srv_conf bad '[server]\n'"$line"'\n' --local-cidr 10.71.0.1/24
    rc=1
    for _ in $(seq 1 10); do
        [ "$(docker inspect -f '{{.State.Running}}' "$P-bad")" = false ] && { rc=0; break; }
        sleep 0.5
    done
    [ "$rc" = 0 ] && docker logs "$P-bad" 2>&1 | grep -qi "invalid.*$want"
    check $? "\"$line\" is refused ($(docker logs "$P-bad" 2>&1 | grep -i invalid | head -1))"
    docker rm -f "$P-bad" >/dev/null
done

echo "== 9. a reload moves clients to the new pool"
srv_conf mv '[server]\nauto_pool=10.80.0.0/24\n' --local-cidr 10.80.0.1/24
sleep 1
atk "$P-mv" MOVER > "$BUILD_CTX/mv1.txt"
grep -q "^MOVER 10.80.0.2/24$" "$BUILD_CTX/mv1.txt"
check $? "the client gets 10.80.0.2 from the first pool"
printf '[server]\nauto_pool=10.81.0.0/24\n' > "$BUILD_CTX/mv2.conf"
docker cp "$BUILD_CTX/mv2.conf" "$P-mv:/etc/camex.conf" >/dev/null
docker kill -s HUP "$P-mv" >/dev/null
sleep 2
atk "$P-mv" MOVER@10.80.0.2 > "$BUILD_CTX/mv2.txt"
grep -q "^MOVER@10.80.0.2 10.81.0.2/24$" "$BUILD_CTX/mv2.txt"
check $? "its next handshake gets an address of the new pool ($(cut -d' ' -f2 "$BUILD_CTX/mv2.txt"))"
docker logs "$P-mv" 2>&1 | grep -q "Auto pool 10.81.0.0/24 is not inside the server's tunnel network"
check $? "the reload reports that the new pool is outside the server's tunnel network"

echo "== 10. TCP: slots stay below FD_SETSIZE"
srv_conf tcp '[server]\nauto_pool=10.90.0.0/16\n' --local-cidr 10.90.0.1/16 --transport tcp
sleep 1
docker logs "$P-tcp" 2>&1 | grep -q "TCP: 65533 client slots reduced to 1024"
check $? "a TCP server with a /16 pool gets 1024 slots"

echo "== 11. the default pool outside the server's tunnel network is reported"
docker logs "$P-srv2" 2>&1 | grep -q "Auto pool 10.200.0.0/24 is not inside the server's tunnel network 10.201.0.1/24"
check $? "--local-cidr 10.201.0.1/24 with the default pool gives a warning"
docker logs "$P-srv1" 2>&1 | grep -q "Client slots: 256, auto pool 10.200.0.0/24"
check $? "without auto_pool= and max_clients=: 256 slots and 10.200.0.0/24"

echo "== 12. slots above the memory lock limit: the server runs, with a warning"
srv_conf nolock '[server]\nauto_pool=10.62.0.0/16\n' --local-cidr 10.62.0.1/16
sleep 1
docker logs "$P-nolock" 2>&1 | grep -q "SECURITY WARNING: 65533 client slots (.* MB) do not fit the memory lock limit"
check $? "the server says the slots do not fit the memory lock limit"
atk "$P-nolock" UNLOCKED > "$BUILD_CTX/nolock.txt"
grep -q "^UNLOCKED 10.62.0.2/16$" "$BUILD_CTX/nolock.txt"
check $? "and still serves clients ($(cut -d' ' -f2 "$BUILD_CTX/nolock.txt"))"

if [ "$FAILS" != 0 ]; then
    echo "--- server 1 log (tail) ---"
    docker logs "$P-srv1" 2>&1 | tail -5
    echo "--- server 2 log (tail) ---"
    docker logs "$P-srv2" 2>&1 | tail -5
    echo "FAIL: $FAILS check(s) failed"
    exit 2
fi
echo "PASS: the auto pool hands out unique addresses of its network; auto_pool= and max_clients= work; unknown IDs can be refused"
