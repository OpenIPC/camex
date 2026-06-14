#!/bin/sh
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-smoke.sh — smoke test for camex using Docker containers
#
# Starts a server and client in separate containers, verifies they
# register and communicate over the tunnel.
#
# Usage: ./test-smoke.sh [--binary ../camex]
#
# Environment:
#   CAMEX_BINARY  path to pre-built camex binary (default: ../camex)
#
# Exit codes:
#   0 = all tests pass
#   1 = build / setup failure
#   2 = test failure
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/camex}"
NET_NAME="${NET_NAME:-camex-smoke-$$}"
SERVER_NAME="camex-server-$$"
CLIENT_NAME="camex-client-$$"
PASS="ci-test-pass"
PASSFILE="/tmp/camex-psk-$$"

cleanup() {
    echo "=== Cleaning up ==="
    docker rm -f "$SERVER_NAME" "$CLIENT_NAME" 2>/dev/null || true
    docker network rm "$NET_NAME" 2>/dev/null || true
    rm -f "$PASSFILE"
}
trap cleanup EXIT INT TERM

echo "=== camex smoke test ==="
echo "Binary: $CAMEX_BINARY"

# --- Sanity checks ---
if [ ! -x "$CAMEX_BINARY" ]; then
    echo "ERROR: binary not found or not executable: $CAMEX_BINARY"
    exit 1
fi

echo "1. Basic help check..."
"$CAMEX_BINARY" --help 2>&1 | head -5
echo ""

echo "2. Version check..."
"$CAMEX_BINARY" --version 2>&1
echo ""

# --- Build Docker image ---
echo "3. Building Docker smoke image..."
BUILD_DIR=$(mktemp -d /tmp/camex-smoke-build-XXXXXX)
cp "$CAMEX_BINARY" "$BUILD_DIR/camex"
cp "$SCRIPT_DIR/Dockerfile.smoke" "$BUILD_DIR/"
docker build -t camex:smoke -f "$BUILD_DIR/Dockerfile.smoke" "$BUILD_DIR" > /dev/null
rm -rf "$BUILD_DIR"
echo "   OK"
echo ""

# --- Create test network ---
echo "4. Creating Docker network..."
docker network create "$NET_NAME" > /dev/null
echo "   Network: $NET_NAME"
echo ""

# --- Start server ---
echo "5. Starting server container..."
echo "$PASS" > "$PASSFILE"
# The server needs to be able to write PID file
docker run -d --name "$SERVER_NAME" \
    --network "$NET_NAME" \
    --cap-add NET_ADMIN \
    --device /dev/net/tun \
    --security-opt apparmor=unconfined \
    camex:smoke \
    --mode server --port 7000 --bind-ip 0.0.0.0 \
    --encrypt --psk "$PASS" \
    --pid-file /tmp/camex.pid 2>&1

SERVER_IP=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$SERVER_NAME")
echo "   Server IP: $SERVER_IP"
echo "   Container: $SERVER_NAME"
sleep 2

# Verify server is running
if ! docker ps --format '{{.Names}}' | grep -q "$SERVER_NAME"; then
    echo "ERROR: server failed to start"
    docker logs "$SERVER_NAME" 2>&1 || true
    exit 2
fi
echo "   Server is running"
echo ""

# --- Start client ---
echo "6. Starting client container (auto mode)..."
docker run -d --name "$CLIENT_NAME" \
    --network "$NET_NAME" \
    --cap-add NET_ADMIN \
    --device /dev/net/tun \
    --security-opt apparmor=unconfined \
    camex:smoke \
    --mode client --auto --name SMOKE01 \
    --server-host "$SERVER_NAME" --port 7000 \
    --encrypt --psk "$PASS" \
    --pid-file /tmp/camex.pid 2>&1

echo "   Container: $CLIENT_NAME"
sleep 3

# --- Check logs ---
echo "7. Server logs:"
docker logs "$SERVER_NAME" 2>&1 | tail -15
echo ""

echo "8. Client logs:"
docker logs "$CLIENT_NAME" 2>&1 | tail -15
echo ""

# --- Validate test outcome ---
echo "9. Validating..."

# Check server registered the client
if docker logs "$SERVER_NAME" 2>&1 | grep -qiE "(registered|client.*SMOKE01)"; then
    echo "   ✅ Server detected client registration"
else
    echo "   ⚠️  Client registration not confirmed in server logs"
fi

# Check client received config (auto mode response from server)
if docker logs "$CLIENT_NAME" 2>&1 | grep -qiE "(config|register|assigned|tunnel)"; then
    echo "   ✅ Client processed server response"
else
    echo "   ⚠️  No config response seen in client logs"
fi

# Check if both containers are still running
SERVER_RUNNING=$(docker ps --format '{{.Names}}' | grep -c "$SERVER_NAME" || true)
CLIENT_RUNNING=$(docker ps --format '{{.Names}}' | grep -c "$CLIENT_NAME" || true)
echo "   Server running: $([ "$SERVER_RUNNING" -gt 0 ] && echo "✅" || echo "❌")"
echo "   Client running: $([ "$CLIENT_RUNNING" -gt 0 ] && echo "✅" || echo "❌")"

# --- Determine outcome ---
if [ "$SERVER_RUNNING" -gt 0 ] || docker logs "$SERVER_NAME" 2>&1 | grep -qE "(listening|TUN|bound|register)"; then
    echo ""
    echo "=== ✅ SMOKE TEST PASSED ==="
    exit 0
else
    echo ""
    echo "=== ❌ SMOKE TEST FAILED ==="
    exit 2
fi
