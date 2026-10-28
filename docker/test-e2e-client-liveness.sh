#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-e2e-client-liveness.sh — regression test for issue #10
#
# Scenario 1 (server restart): a manual-mode encrypted UDP client must
# recover after the server is restarted. Before the fix the restarted
# server's sequence numbers started from 0 and the client rejected them as
# replays until the server had sent as many packets as the client had
# received in the old session (for a long session: effectively forever).
#
# Scenario 2 (server gone): the client must notice that the server stopped
# answering (docker pause) and log "Connection lost", then recover after
# the server comes back. Before the fix a successful UDP send counted as a
# sign of life, so the client never noticed.
#
# Scenario 3 (idle, keepalive longer than the timeout): a manual client
# with --keepalive 10 --server-timeout 5 must stay connected while idle.
# Without the keepalive interval capped at half the timeout it timed out
# between probes and reconnected every few seconds.
#
# Scenario 4 (one-way loss): packets from the server are dropped at the
# client (iptables) while the client's own packets still reach the server.
# Sending works, nothing comes back: the client must still log "Connection
# lost" and recover once the path is back.
#
# Scenario 5 (#143): a server with client_timeout=60 expires a silent
# client after 60 s (not before 50 s, by 70 s), and keeps one started with
# --keepalive 3600, whose keepalive is capped at 20 s.
#
# Scenario 2 runs right after scenario 1's reconnect: whether the server
# answers keepalives must survive the reconnect, or silence detection
# stays off until the next answered keepalive.
#
# Usage: ./test-e2e-client-liveness.sh
#   CAMEX_BINARY=path/to/camex  binary to test (default: ../camex)
#   KEEP=1                      keep containers for debugging
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/camex}"
IMAGE="camex:e2e-live"
NET_NAME="camex-live-$$"
SRV="camex-live-srv-$$"
CLI="camex-live-cli-$$"
IDLE="camex-live-idle-$$"
SRV2="camex-live-srv2-$$"
QUIET="camex-live-quiet-$$"
LONG="camex-live-long-$$"
BUILD_CTX="$(mktemp -d)"
PSK="liveness-test"

cleanup() {
    docker rm -f "$SRV" "$CLI" "$IDLE" "$SRV2" "$QUIET" "$LONG" >/dev/null 2>&1 || true
    docker network rm "$NET_NAME" >/dev/null 2>&1 || true
    rm -rf "$BUILD_CTX"
}
[ -n "$KEEP" ] || trap cleanup EXIT
# Ctrl+C or a cancelled job ends the script; EXIT then cleans up once (#125)
trap 'exit 130' INT
trap 'exit 143' TERM

fail() {
    echo "FAIL: $*"
    echo "--- client log (tail) ---"
    docker logs "$CLI" 2>&1 | tail -15 || true
    exit 2
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
        iputils-ping iptables >/dev/null && rm -rf /var/lib/apt/lists/*
COPY camex /usr/local/bin/camex
COPY server.key /etc/camex/server.key
RUN printf '#!/bin/sh\nmkdir -p /dev/net\n[ -e /dev/net/tun ] || mknod /dev/net/tun c 10 200\nexec /usr/local/bin/camex "$@"\n' \
        > /usr/local/bin/camex-run && chmod +x /usr/local/bin/camex-run
ENTRYPOINT ["/usr/local/bin/camex-run"]
EOF
docker build -q -t "$IMAGE" "$BUILD_CTX" >/dev/null
docker network create "$NET_NAME" >/dev/null

# wait_ping <seconds>: succeed as soon as the client reaches the server
wait_ping() {
    local i
    for i in $(seq 1 "$1"); do
        if docker exec "$CLI" ping -c1 -W1 10.200.0.1 >/dev/null 2>&1; then
            echo "   reachable after ${i}s"
            return 0
        fi
        sleep 1
    done
    return 1
}

start_server() {
    docker run -d --name "$SRV" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
        "$IMAGE" --mode server --port 5800 --local-cidr 10.200.0.1/24 \
        $SERVER_KEY_ARGS --psk "$PSK" >/dev/null
}

echo "== start server and manual encrypted client"
start_server
sleep 1
docker run -d --name "$CLI" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    "$IMAGE" --mode client --local-cidr 10.200.0.2/24 --gateway-ip 10.200.0.1 \
    --server-host "$SRV" --port 5800 --server-pubkey "$SERVER_PUBKEY" --psk "$PSK" \
    --keepalive 2 --server-timeout 6 >/dev/null
wait_ping 20 || fail "client never reached the server"

# Let at least one keepalive round-trip happen, and move the client's
# receive window well ahead (a real session has received many packets).
sleep 5
docker exec "$CLI" ping -q -c 300 -i 0.01 10.200.0.1 >/dev/null 2>&1 || true

echo "== scenario 1: restart the server"
docker restart -t 1 "$SRV" >/dev/null
wait_ping 60 || fail "client did not recover after a server restart"

echo "== scenario 2: pause the server (it stops answering)"
# Count only messages logged after the pause: scenario 1 may already have
# logged "Connection lost" during the restart.
seen=$(docker logs "$CLI" 2>&1 | grep -c "Connection lost" || true)
docker pause "$SRV" >/dev/null
lost=0
for _ in $(seq 1 30); do
    now=$(docker logs "$CLI" 2>&1 | grep -c "Connection lost" || true)
    if [ "$now" -gt "$seen" ]; then
        lost=1; break
    fi
    sleep 1
done
docker unpause "$SRV" >/dev/null
[ "$lost" = 1 ] || fail "client did not detect that the server stopped answering"
echo "   client detected the lost server"
wait_ping 60 || fail "client did not recover after the server came back"

echo "== scenario 4: one-way loss (server -> client dropped, client -> server works)"
seen=$(docker logs "$CLI" 2>&1 | grep -c "Connection lost" || true)
docker exec "$CLI" iptables -I INPUT -p udp --sport 5800 -j DROP ||
    fail "cannot add the iptables rule in the client container"
lost=0
for _ in $(seq 1 30); do
    now=$(docker logs "$CLI" 2>&1 | grep -c "Connection lost" || true)
    if [ "$now" -gt "$seen" ]; then
        lost=1; break
    fi
    sleep 1
done
docker exec "$CLI" iptables -D INPUT -p udp --sport 5800 -j DROP
[ "$lost" = 1 ] || fail "client did not notice that nothing came back from the server"
echo "   client detected the one-way loss"
wait_ping 60 || fail "client did not recover after the one-way loss ended"

echo "== scenario 3: idle client, keepalive (10s) longer than the timeout (5s)"
docker rm -f "$CLI" >/dev/null
docker run -d --name "$IDLE" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    "$IMAGE" --mode client --local-cidr 10.200.0.3/24 --gateway-ip 10.200.0.1 \
    --server-host "$SRV" --port 5800 --server-pubkey "$SERVER_PUBKEY" --psk "$PSK" \
    --keepalive 10 --server-timeout 5 >/dev/null
CLI="$IDLE"
wait_ping 20 || fail "idle client never reached the server"
sleep 20
lost=$(docker logs "$IDLE" 2>&1 | grep -c "Connection lost" || true)
[ "$lost" = 0 ] || fail "idle client timed out between keepalives ($lost times)"
echo "   no reconnects while idle"

echo "== scenario 5: client_timeout=60 (#143), made by a reload of client_timeout=120:"
echo "   a silent client goes after 60 s, not 120; one with --keepalive 3600 stays"
echo "   (its keepalive is capped at 20 s)"
docker rm -f "$IDLE" >/dev/null
docker run -d --name "$SRV2" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    --entrypoint /bin/sh "$IMAGE" -c 'printf "[server]\nclient_timeout=120\n" > /tmp/c.conf &&
        exec camex-run --mode server --port 5800 --local-cidr 10.210.0.1/24 \
        --config /tmp/c.conf '"$SERVER_KEY_ARGS"' --psk "'"$PSK"'"' >/dev/null
sleep 1
for c in "$QUIET:10.210.0.2:2:6" "$LONG:10.210.0.3:3600:3600"; do
    IFS=: read -r name addr ka to <<EOF2
$c
EOF2
    docker run -d --name "$name" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
        "$IMAGE" --mode client --local-cidr "$addr/24" --gateway-ip 10.210.0.1 \
        --server-host "$SRV2" --port 5800 --server-pubkey "$SERVER_PUBKEY" --psk "$PSK" \
        --keepalive "$ka" --server-timeout "$to" >/dev/null
done
CLI="$QUIET"
for _ in $(seq 1 20); do
    docker exec "$QUIET" ping -c1 -W1 10.210.0.1 >/dev/null 2>&1 &&
        docker exec "$LONG" ping -c1 -W1 10.210.0.1 >/dev/null 2>&1 && break
    sleep 1
done
# The reload applies the new value: with 120 left nobody would expire below
docker exec "$SRV2" sh -c 'printf "[server]\nclient_timeout=60\n" > /tmp/c.conf'
docker kill -s HUP "$SRV2" >/dev/null
for _ in $(seq 1 10); do
    docker logs "$SRV2" 2>&1 | grep -q "Reloading config" && break
    sleep 0.5
done
sleep 1
docker logs "$SRV2" 2>&1 | grep -q "Reloading config" || fail "the server did not reload"
docker logs "$SRV2" 2>&1 | grep -q "Keeping previous server configuration" &&
    fail "the reload of client_timeout=60 was refused"
docker pause "$QUIET" >/dev/null
sleep 50
docker logs "$SRV2" 2>&1 | grep -q "Expired client 10.210.0.2" &&
    { docker unpause "$QUIET" >/dev/null; fail "the silent client expired before 50 s"; }
gone=0
for i in $(seq 1 20); do
    docker logs "$SRV2" 2>&1 | grep -q "Expired client 10.210.0.2" && { gone=1; break; }
    sleep 1
done
docker unpause "$QUIET" >/dev/null
[ "$gone" = 1 ] || fail "the silent client did not expire within 70 s of the reloaded client_timeout=60"
echo "   the silent client expired after $((50 + i)) s"
docker logs "$SRV2" 2>&1 | grep -q "Expired client 10.210.0.3" &&
    fail "the client with --keepalive 3600 expired"
docker exec "$LONG" ping -c1 -W2 10.210.0.1 >/dev/null 2>&1 ||
    fail "the client with --keepalive 3600 lost its tunnel"
echo "   the client with --keepalive 3600 stays"

echo "PASS: client detects a lost server and recovers after a restart; idle client stays up; client_timeout= expires"
