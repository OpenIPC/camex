#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-e2e-throughput.sh — bulk data through the tunnel, both ways, over UDP
# and TCP, on a link with a real round trip (#180)
#
# A server (UDP and TCP) and a client per transport, each container with a
# netem delay on its link (30 ms each way, 60 ms round trip). A TCP stream
# inside the tunnel carries SIZE_KB from the server's side to the client
# and back; every transfer must finish within its time limit (MIN_KBPS).
# Before #180 the server sent at most one frame per TCP connection while
# its socket was busy and dropped the rest: server-to-client over TCP
# crawled at ~10 KB/s.
#
# Usage: ./test-e2e-throughput.sh
#   CAMEX_BINARY=path/to/camex  binary to test (default: ../camex)
#   SIZE_KB=4096                data per transfer
#   MIN_KBPS=300                slowest accepted rate
#   DELAY_MS=30                 one-way delay on each container's link
#   KEEP=1                      keep containers for debugging
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/camex}"
IMAGE="camex:e2e-throughput"
P="camex-tput-$$"
NET_NAME="$P-net"
BUILD_CTX="$(mktemp -d)"
PSK="throughput-test"
SIZE_KB="${SIZE_KB:-4096}"
MIN_KBPS="${MIN_KBPS:-300}"
DELAY_MS="${DELAY_MS:-30}"
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
cat > "$BUILD_CTX/sink.py" <<'PY'
# Accept one TCP stream on port 9000, read it to the end, report bytes
import socket, sys, time
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", 9000)); s.listen(1); s.settimeout(float(sys.argv[1]))
c, _ = s.accept(); c.settimeout(float(sys.argv[1])); n = 0
try:
    while True:
        d = c.recv(65536)
        if not d:
            break
        n += len(d)
except Exception:
    pass
print(n)
PY
cat > "$BUILD_CTX/source.py" <<'PY'
# Send <kb> KiB to <host>:9000 within <limit> s in all; print the time it
# took, or "timeout" / "error" (the deadline covers the whole transfer)
import socket, sys, time
host, kb, limit = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
t = time.time(); total = kb * 1024; buf = b"x" * 65536; n = 0
def left():
    r = limit - (time.time() - t)
    if r <= 0:
        raise TimeoutError
    return r
try:
    s = socket.create_connection((host, 9000), timeout=left())
    while n < total:
        s.settimeout(left())
        k = s.send(buf[:min(65536, total - n)]); n += k
    s.shutdown(socket.SHUT_WR)
    s.settimeout(left())
    s.recv(1)
except (TimeoutError, socket.timeout):
    print("timeout"); sys.exit(1)
except Exception:
    print("error"); sys.exit(1)
print("%.2f" % (time.time() - t))
PY
cat > "$BUILD_CTX/Dockerfile" <<'DOCKERFILE'
FROM ubuntu:24.04
ENV CAMEX_POOL_HOLD=0
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
        python3-minimal iproute2 iputils-ping >/dev/null && rm -rf /var/lib/apt/lists/*
COPY camex /usr/local/bin/camex
COPY server.key /etc/camex/server.key
COPY sink.py source.py /usr/local/bin/
RUN printf '#!/bin/sh\nmkdir -p /dev/net\n[ -e /dev/net/tun ] || mknod /dev/net/tun c 10 200\ntc qdisc add dev eth0 root netem delay ${DELAY_MS:-30}ms 2>/dev/null\nexec /usr/local/bin/camex "$@"\n' \
        > /usr/local/bin/camex-run && chmod +x /usr/local/bin/camex-run
ENTRYPOINT ["/usr/local/bin/camex-run"]
DOCKERFILE
docker build -q -t "$IMAGE" "$BUILD_CTX" >/dev/null 2>&1
docker network create "$NET_NAME" >/dev/null
set +e   # checks below report failures themselves

docker run -d --name "$P-srv" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    -e DELAY_MS="$DELAY_MS" "$IMAGE" --mode server --port 5800 \
    --local-cidr 10.200.0.1/24 $SERVER_KEY_ARGS --psk "$PSK" >/dev/null
sleep 1
for tr in udp tcp; do
    docker run -d --name "$P-$tr" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
        -e DELAY_MS="$DELAY_MS" "$IMAGE" --mode client --auto --name "TPUT-$tr" \
        --server-host "$P-srv" --port 5800 --transport "$tr" \
        --server-pubkey "$SERVER_PUBKEY" --psk "$PSK" --keepalive 2 >/dev/null
done

tun_addr() {
    docker exec "$1" ip -4 -o addr show 2>/dev/null |
        awk '$4 ~ /^10\.200\./ {sub("/.*","",$4); print $4}'
}
for _ in $(seq 1 20); do
    [ -n "$(tun_addr "$P-udp")" ] && [ -n "$(tun_addr "$P-tcp")" ] && break
    sleep 1
done
rtt=$(docker exec "$P-udp" ping -c 3 -q 10.200.0.1 2>/dev/null | sed -n 's|.*= [0-9.]*/\([0-9.]*\)/.*|\1|p')
echo "== tunnels up (round trip through the tunnel ${rtt:-?} ms), ${SIZE_KB} KiB each way, at least ${MIN_KBPS} KB/s"

limit=$(( SIZE_KB / MIN_KBPS + 3 ))
transfer() {  # transfer <from container> <to container> <to address> <label>
    docker exec -d "$2" sh -c "python3 /usr/local/bin/sink.py $((limit + 5)) > /tmp/sink.out 2>&1"
    sleep 1
    t=$(docker exec "$1" python3 /usr/local/bin/source.py "$3" "$SIZE_KB" "$limit")
    sleep 1
    got=$(docker exec "$2" cat /tmp/sink.out 2>/dev/null)
    rate=$(awk -v g="${got:-0}" -v t="${t:-0}" 'BEGIN { if (t + 0 > 0) printf "%d", g / 1024 / t; else print 0 }')
    # All of it, and at least MIN_KBPS: a complete but slow transfer fails
    [ "${got:-0}" = $((SIZE_KB * 1024)) ] && [ "${rate:-0}" -ge "$MIN_KBPS" ]
    check $? "$4: $((${got:-0} / 1024)) of $SIZE_KB KiB in ${t:-?} s (~$rate KB/s, at least $MIN_KBPS)"
}
for tr in udp tcp; do
    addr=$(tun_addr "$P-$tr")
    transfer "$P-srv" "$P-$tr" "$addr" "$tr server to client"
    transfer "$P-$tr" "$P-srv" 10.200.0.1 "$tr client to server"
done

docker exec "$P-srv" true
check $? "the server is still running"

if [ "$FAILS" -ne 0 ]; then
    echo "FAIL: $FAILS check(s) failed"
    exit 1
fi
echo "PASS: bulk data through the tunnel both ways over UDP and TCP"
