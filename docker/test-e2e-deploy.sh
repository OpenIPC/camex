#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-e2e-deploy.sh — the camex server in Docker of deploy/ (#205): the
# image as deploy/Dockerfile builds it, the first start as entrypoint.sh
# makes it
#
#  1. The image checks the binary: a wrong CAMEX_SHA256 stops the build, a
#     CAMEX_RELEASE of its own without CAMEX_SHA256 too; the right one
#     builds (the release served from a container of this test)
#  2. The first start: the key and camex.conf mode 0600; CAMEX_POOL with an
#     address inside the pool gives its network, the server at the first
#     address; CAMEX_DOMAIN gives domain= and web_listen=yes; the network
#     has a label, and the log has neither its name nor the psk
#  3. A client joins with the name from camex.conf and the key the log
#     prints, gets an address of the network's pool and reaches its gateway
#  4. The start page answers on 127.0.0.1:5801 in the container
#
# Usage: ./test-e2e-deploy.sh
#   CAMEX_BINARY=path  a static x86_64 camex (default: ../build/camex-x86_64,
#                      ci/build-musl.sh x86_64 build)
#   KEEP=1             keep containers for debugging
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/build/camex-x86_64}"
IMAGE="camex:e2e-deploy"
P="camex-deploy-$$"
NET_NAME="$P-net"
BUILD_CTX="$(mktemp -d)"
FAILS=0

cleanup() {
    docker ps -a --format '{{.Names}}' | grep "^$P-" | xargs -r docker rm -f >/dev/null 2>&1 || true
    docker network rm "$NET_NAME" >/dev/null 2>&1 || true
    docker rmi -f "$IMAGE" >/dev/null 2>&1 || true
    rm -rf "$BUILD_CTX"
}
[ -n "$KEEP" ] || trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

check() {   # check <condition-exit-code> <message>
    if [ "$1" = 0 ]; then echo "   ok: $2"; else echo "   FAIL: $2"; FAILS=$((FAILS + 1)); fi
}

[ -x "$CAMEX_BINARY" ] || { echo "camex binary not found: $CAMEX_BINARY (ci/build-musl.sh x86_64)"; exit 1; }
SHA=$(sha256sum "$CAMEX_BINARY" | cut -d' ' -f1)

echo "== 1. the image checks the binary"
# The release: the binary as camex-x86_64 on a web server of its own
# (no -full: the Dockerfile falls back), published on the daemon's host for
# docker build --network host
docker create --name "$P-rel" -p 127.0.0.1::8000 alpine:3.22 \
    sh -c 'apk add -q --no-progress busybox-extras >/dev/null && exec httpd -f -p 8000 -h /www' >/dev/null
mkdir -p "$BUILD_CTX/www"
cp "$CAMEX_BINARY" "$BUILD_CTX/www/camex-x86_64"
docker cp "$BUILD_CTX/www" "$P-rel:/www" >/dev/null
docker start "$P-rel" >/dev/null
for _ in $(seq 1 60); do   # apk first
    docker exec "$P-rel" sh -c 'command -v httpd' >/dev/null 2>&1 && break
    sleep 0.5
done
PORT=$(docker port "$P-rel" 8000/tcp | head -n 1 | sed 's/.*://')
REL="http://127.0.0.1:$PORT"
cp "$REPO_DIR"/deploy/Dockerfile "$REPO_DIR"/deploy/entrypoint.sh "$BUILD_CTX/"
build() {   # build <build args...>: the image, its output in build.log
    docker build --network host -t "$IMAGE" "$@" "$BUILD_CTX" > "$BUILD_CTX/build.log" 2>&1
}
set +e
build --build-arg CAMEX_RELEASE="$REL" \
    --build-arg CAMEX_SHA256=0000000000000000000000000000000000000000000000000000000000000000
r=$?
[ $r != 0 ] && grep -q "camex-x86_64: SHA-256 $SHA, not 0000" "$BUILD_CTX/build.log"
check $? "a wrong CAMEX_SHA256 stops the build, and says so"
build --build-arg CAMEX_RELEASE="$REL"
r=$?
[ $r != 0 ] && grep -q "needs CAMEX_SHA256" "$BUILD_CTX/build.log"
check $? "a release of its own without CAMEX_SHA256 stops the build"
build --build-arg CAMEX_RELEASE="$REL" --build-arg CAMEX_SHA256="$SHA"
r=$?
[ $r = 0 ]
check $r "the right CAMEX_SHA256 builds the image"
[ $r = 0 ] || { tail -20 "$BUILD_CTX/build.log"; exit 1; }

echo "== 2. the first start"
docker network create "$NET_NAME" >/dev/null
docker run -d --name "$P-srv" --network "$NET_NAME" --cap-add NET_ADMIN \
    --device /dev/net/tun -e CAMEX_POOL=10.60.1.7/22 -e CAMEX_DOMAIN=cams.test \
    "$IMAGE" >/dev/null
for _ in $(seq 1 20); do
    docker logs "$P-srv" 2>&1 | grep -q "Tunnel up" && break
    sleep 0.5
done
docker exec "$P-srv" sh -c 'stat -c "%a %n" /etc/camex/server.key /etc/camex/camex.conf' > "$BUILD_CTX/modes"
[ "$(grep -c '^600 ' "$BUILD_CTX/modes")" = 2 ]
check $? "server.key and camex.conf are mode 0600: $(tr '\n' ' ' < "$BUILD_CTX/modes")"
docker exec "$P-srv" cat /etc/camex/camex.conf > "$BUILD_CTX/camex.conf"
grep -qx "local_cidr=10.60.0.1/22" "$BUILD_CTX/camex.conf" &&
    grep -qx "auto_pool=10.60.0.0/22" "$BUILD_CTX/camex.conf"
check $? "CAMEX_POOL=10.60.1.7/22: the pool 10.60.0.0/22, the server at 10.60.0.1"
grep -qx "domain=cams.test" "$BUILD_CTX/camex.conf" && grep -qx "web_listen=yes" "$BUILD_CTX/camex.conf"
check $? "CAMEX_DOMAIN: domain= and web_listen=yes"
NAME=$(sed -n 's/^\[network \(.*\)\]$/\1/p' "$BUILD_CTX/camex.conf")
PSK=$(sed -n 's/^psk=//p' "$BUILD_CTX/camex.conf")
docker logs "$P-srv" > "$BUILD_CTX/srv.log" 2>&1
[ ${#NAME} = 20 ] && [ -n "$PSK" ] &&
    ! grep -qF "$NAME" "$BUILD_CTX/srv.log" && ! grep -qF "${PSK#hex:}" "$BUILD_CTX/srv.log"
check $? "the log has neither the network's name nor the psk"
PUB=$(sed -n 's/.*--server-pubkey \([^ ]*\).*/\1/p' "$BUILD_CTX/srv.log" | head -n 1)
[ -n "$PUB" ]
check $? "the log says how clients join, with the server's key"
docker run --rm --network "$NET_NAME" --cap-add NET_ADMIN --device /dev/net/tun \
    -e CAMEX_POOL=10.0.0.0/33 "$IMAGE" > "$BUILD_CTX/bad.log" 2>&1
r=$?
[ $r != 0 ] && grep -q "CAMEX_POOL=10.0.0.0/33 is not" "$BUILD_CTX/bad.log"
check $? "a CAMEX_POOL that is no pool stops the first start"

echo "== 3. a client joins"
docker run -d --name "$P-cli" --network "$NET_NAME" --cap-add NET_ADMIN \
    --device /dev/net/tun --entrypoint camex "$IMAGE" --server-host "$P-srv" \
    --network "$NAME" --server-pubkey "$PUB" --name CAM-D >/dev/null
A=
for _ in $(seq 1 30); do
    A=$(docker exec "$P-cli" ip -4 -o addr show 2>/dev/null | awk '$2 ~ /^camex|^tun/ { print $4; exit }')
    [ -n "$A" ] && break
    sleep 0.5
done
# The network's pool, as the server logs it by the label
NP=$(docker logs "$P-srv" 2>&1 | sed -n 's/.*Network main: pool \([0-9.]*\)\.0\.0\/16.*/\1/p' | head -n 1)
case "$A" in "$NP".*/16) r=0 ;; *) r=1 ;; esac
[ -n "$NP" ] && [ $r = 0 ]
check $? "the client has an address of the network's pool: ${A:-none} (${NP:-?}.0.0/16)"
docker exec "$P-cli" ping -c 2 -W 2 "$NP.0.1" >/dev/null 2>&1
check $? "the client reaches the network's gateway"

echo "== 4. the start page"
docker exec "$P-srv" wget -q -O - http://127.0.0.1:5801/ 2>/dev/null | grep -q '<meta name="camex-version"'
check $? "camex's start page answers on 127.0.0.1:5801"

echo
if [ "$FAILS" != 0 ]; then
    echo "--- server log (tail) ---"
    docker logs "$P-srv" 2>&1 | tail -10
    echo "FAIL: $FAILS check(s) failed"
    exit 1
fi
echo "PASS: the camex server of deploy/ builds, checks its binary, starts and takes a client"
