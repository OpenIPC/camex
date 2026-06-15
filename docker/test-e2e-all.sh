#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-e2e-all.sh — full E2E test for 4 modes (UDP/TCP × plain/encrypted)
#
# Tests:  UDP plain, TCP plain, UDP encrypted, TCP encrypted
# Each mode runs for 120 seconds, logs are analyzed for reconnects.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/camex}"
NET_NAME="camex-e2e-$$"
SERVER_NAME="camex-srv-$$"
CLIENT_NAME="camex-cli-$$"
PASS="e2e-test-pass"
DURATION=60  # seconds to hold connection
LOG_DIR="/tmp/camex-e2e-logs-$$"
FAILED_MODES=""

# ── helpers ──────────────────────────────────────────────────────────

cleanup() {
    echo ""
    echo "=== Cleanup ==="
    docker rm -f "$SERVER_NAME" "$CLIENT_NAME" 2>/dev/null || true
    docker network rm "$NET_NAME" 2>/dev/null || true
    rm -rf "$LOG_DIR"
}
trap cleanup EXIT INT TERM

log_section() {
    echo ""
    echo "══════════════════════════════════════════════════════════"
    echo "  $1"
    echo "══════════════════════════════════════════════════════════"
}

run_docker() {
    local role="$1" name="$2" extra="$3"
    shift 3
    echo "   ▶ Starting $role container..."
    cid=$(docker run -d --name "$name" \
        --network "$NET_NAME" \
        --cap-add NET_ADMIN \
        --device /dev/net/tun:/dev/net/tun \
        --security-opt seccomp=unconfined \
        $extra \
        camex:e2e \
        "$@" 2>&1) || {
        echo "   ✗ docker run failed: $cid"
        return 1
    }
    echo "   Container: $name (${cid:0:12}…)"
    sleep 2
    if ! docker ps --format '{{.Names}}' | grep -qx "$name"; then
        ec=$(docker inspect "$name" --format='{{.State.ExitCode}}' 2>/dev/null || echo "?")
        echo "   ✗ $role exited (code=$ec). Logs:"
        docker logs "$name" 2>&1 || true
        return 1
    fi
    return 0
}

analyze_logs() {
    local mode="$1" logfile="$2"
    local reconnect_count dead_count ok=0

    echo ""
    echo "   ── Log analysis for $mode ──"

    # Count reconnects / restarts
    reconnect_count=$(grep -cE "(reconnect|re-register|RE-REGISTER|\"Server started\")" "$logfile" 2>/dev/null | tr -d '[:space:]')
    dead_count=$(grep -cE "(exit|dead|dropped|ERROR|FAIL)" "$logfile" 2>/dev/null | tr -d '[:space:]')
    [ -z "$reconnect_count" ] && reconnect_count=0
    [ -z "$dead_count" ] && dead_count=0

    echo "   Reconnect/restart events: $reconnect_count"
    echo "   Error/dead events:        $dead_count"

    # Check for cyclic reconnects
    if [ "$reconnect_count" -gt 2 ] 2>/dev/null; then
        echo "   ⚠  WARNING: >2 reconnect events — possible cyclic reconnect loop!"
        ok=1
    fi

    # Check both containers alive at end
    if docker ps --format '{{.Names}}' | grep -qx "$SERVER_NAME" && \
       docker ps --format '{{.Names}}' | grep -qx "$CLIENT_NAME"; then
        echo "   ✓ Both containers alive at end"
    else
        echo "   ✗ Container(s) died during test"
        ok=1
    fi

    # Check encryption handshake if encrypted mode
    if echo "$mode" | grep -qi "encrypt"; then
        if grep -qi "fingerprint" "$logfile" 2>/dev/null; then
            echo "   ✓ Encryption fingerprint detected"
        else
            echo "   ✗ No encryption fingerprint — possible handshake failure"
            ok=1
        fi
    fi

    return $ok
}

run_test() {
    local mode="$1" transport="$2" encrypt="$3"
    local logfile="$LOG_DIR/mode-${mode}.log"
    local extra_args=""
    local transport_str=""

    if [ "$transport" = "tcp" ]; then
        transport_str="TCP"
        extra_args="--transport tcp"
    else
        transport_str="UDP"
    fi

    log_section "Mode: $transport_str $([ "$encrypt" = "encrypt" ] && echo "ENCRYPTED" || echo "PLAINTEXT")"

    if [ "$encrypt" = "encrypt" ]; then
        extra_args="$extra_args --encrypt --psk $PASS"
    fi

    # Start server
    run_docker "Server" "$SERVER_NAME" "" \
        --mode server --port 5800 --bind-ip 0.0.0.0 \
        $extra_args \
        --pid-file /tmp/camex.pid || { FAILED_MODES="$FAILED_MODES $mode"; return 1; }

    SERVER_IP=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$SERVER_NAME")
    echo "   Server IP: $SERVER_IP"

    # Start client
    run_docker "Client" "$CLIENT_NAME" "" \
        --mode client --auto --name "${transport_str}01" \
        --server-host "$SERVER_NAME" --port 5800 \
        $extra_args \
        --pid-file /tmp/camex.pid || { FAILED_MODES="$FAILED_MODES $mode"; return 1; }

    if [ "$transport" = "tcp" ]; then
        echo "   ▶ TCP mode: extra settle time…"
        sleep 3
    fi

    echo "   ▶ Holding connection for ${DURATION}s…"
    sleep "$DURATION"

    # Collect logs
    {
        echo "=== SERVER LOGS ==="
        docker logs "$SERVER_NAME" 2>&1
        echo ""
        echo "=== CLIENT LOGS ==="
        docker logs "$CLIENT_NAME" 2>&1
    } > "$logfile"

    analyze_logs "$mode" "$logfile" || FAILED_MODES="$FAILED_MODES $mode"

    # Check for "Dropped packet" spam (rate-limit fix verification)
    local drop_count
    drop_count=$(grep -c "Dropped" "$logfile" 2>/dev/null | tr -d '[:space:]')
    [ -z "$drop_count" ] && drop_count=0
    echo "   'Dropped packet' count:   $drop_count"
    if [ "$drop_count" -gt 10 ]; then
        echo "   ⚠  WARNING: excessive dropped packets — possible regression"
    fi

    # Teardown containers for this mode
    docker rm -f "$SERVER_NAME" "$CLIENT_NAME" 2>/dev/null || true
    sleep 2
}

# ── main ──────────────────────────────────────────────────────────────

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  camex E2E Full Matrix Test — 4 modes × ${DURATION}s each"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Binary: $CAMEX_BINARY"
echo ""

# Sanity checks
if [ ! -x "$CAMEX_BINARY" ]; then
    echo "ERROR: binary not found: $CAMEX_BINARY"
    exit 1
fi
echo "Version: $("$CAMEX_BINARY" --version 2>&1)"
echo ""

# Build Docker image
echo "▶ Building Docker image…"
BUILD_DIR=$(mktemp -d /tmp/camex-e2e-build-XXXXXX)
cp "$CAMEX_BINARY" "$BUILD_DIR/camex"
cp "$SCRIPT_DIR/Dockerfile.smoke" "$BUILD_DIR/"
docker build -t camex:e2e -f "$BUILD_DIR/Dockerfile.smoke" "$BUILD_DIR" > /dev/null 2>&1
rm -rf "$BUILD_DIR"
echo "  OK"

# Create log dir
mkdir -p "$LOG_DIR"

# Create network
echo "▶ Creating Docker network…"
docker network create "$NET_NAME" > /dev/null
echo "  Network: $NET_NAME"

# ─── Mode 1: UDP plain ────────────────────────────────────────────────
run_test "udp-plain" "udp" "plain"

# ─── Mode 2: TCP plain ────────────────────────────────────────────────
run_test "tcp-plain" "tcp" "plain"

# ─── Mode 3: UDP encrypted ────────────────────────────────────────────
run_test "udp-encrypt" "udp" "encrypt"

# ─── Mode 4: TCP encrypted ────────────────────────────────────────────
run_test "tcp-encrypt" "tcp" "encrypt"

# ── Summary ───────────────────────────────────────────────────────────
echo ""
echo "══════════════════════════════════════════════════════════"
echo "  FULL RESULTS"
echo "══════════════════════════════════════════════════════════"

echo ""
echo "Mode               Status"
echo "────────────────── ──────"
for mode in udp-plain tcp-plain udp-encrypt tcp-encrypt; do
    if echo " $FAILED_MODES " | grep -q " $mode "; then
        printf "%-18s ✗ FAILED\n" "$mode"
    else
        printf "%-18s ✓ PASSED\n" "$mode"
    fi
done

echo ""
passed=$((4 - $(echo "$FAILED_MODES" | wc -w)))
echo "Total: $passed / 4 passed"
echo "Logs:  $LOG_DIR"

if [ -z "$FAILED_MODES" ]; then
    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "  ALL E2E TESTS PASSED"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    exit 0
else
    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "  SOME TESTS FAILED: $FAILED_MODES"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    exit 2
fi
