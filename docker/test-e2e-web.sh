#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-e2e-web.sh — the start page and nginx's lookup served by camex
# itself (#203): web_listen = yes, examples/nginx-camex.conf with
# auth_request, no map file and no sync script
#
# 1. Two clients behind nginx: each name gets its own client's page, an
#    unknown name the offline page; nginx is never reloaded.
# 2. A client with --web-port 8080 is reached on that port.
# 3. The start page through nginx: its password, both clients, links by
#    name; /resolve is not reachable through it, nor from another host.
# 4. A client that goes away: its name gets the offline page at once.
# 5. A second server on the same host: its clients are on the first one's
#    page and reached by name; web_scope = self leaves them out.
# 6. /status.json: both servers.
# 7. A page on the LAN without nginx: camex does not start without
#    web_password=; with it 401 without the password, 200 with it, links
#    to the tunnel addresses; 5 wrong passwords: 429.
# 8. Requests it refuses: POST 405, a head too large 431, another path 404.
# 9. A 2 MB upload through nginx to a device; /resolve answers any method;
#    the page's script refreshes it in place (jsdom).
#
# Usage: ./test-e2e-web.sh
#   CAMEX_BINARY=path/to/camex  binary to test (default: ../camex)
#   KEEP=1                      keep containers for debugging
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/camex}"
IMAGE="camex:e2e-web"
P="camex-web-$$"
NET_NAME="$P-net"
BUILD_CTX="$(mktemp -d)"
PSK="web-test"
DOMAIN="openipc.click"
PAGE_PW="e2e-page-pw"
LAN_PW="e2e-lan-pw"
FAILS=0

cleanup() {
    docker ps -a --format '{{.Names}}' | grep "^$P-" | xargs -r docker rm -f >/dev/null 2>&1 || true
    docker network rm "$NET_NAME" >/dev/null 2>&1 || true
    rm -rf "$BUILD_CTX"
}
[ -n "$KEEP" ] || trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

check() {   # check <condition-exit-code> <message>
    if [ "$1" = 0 ]; then echo "   ok: $2"; else echo "   FAIL: $2"; FAILS=$((FAILS + 1)); fi
}

[ -x "$CAMEX_BINARY" ] || { echo "camex binary not found: $CAMEX_BINARY"; exit 1; }
"$CAMEX_BINARY" --version 2>&1 | grep -q '^features:.* web' ||
    { echo "$CAMEX_BINARY has no start page (CAMEX_WEB=0)"; exit 1; }

cp "$CAMEX_BINARY" "$BUILD_CTX/camex"
sed "s/openipc\.click/$DOMAIN/g" "$REPO_DIR/examples/nginx-camex.conf" > "$BUILD_CTX/nginx-camex.conf"
. "$SCRIPT_DIR/lib-keys.sh"
gen_server_key "$BUILD_CTX" "$CAMEX_BINARY" || { echo "cannot create a server key"; exit 1; }
LAN_HASH=$(printf '%s\n' "$LAN_PW" | "$CAMEX_BINARY" --hash-password | sed -n 's/^web_password=//p')
[ -n "$LAN_HASH" ] || { echo "camex --hash-password gave nothing"; exit 1; }
printf '[server]\ndomain=%s\nweb_listen=yes\n[client CAM-Z]\nname=Gate\n' "$DOMAIN" \
    > "$BUILD_CTX/camex.conf"
# The second server on the same host: its own pool, no page of its own
# its devices' web port, 8090: on its own records, not the first server's 80
printf '[server]\ndomain=%s\nauto_pool=10.201.0.0/24\ndevice_port=8090\n' "$DOMAIN" > "$BUILD_CTX/camex2.conf"
echo "$PAGE_PW" > "$BUILD_CTX/page-pw"
# A device's web server: files of the current directory, and a POST read to its
# end before the answer (python's http.server answers 501 at once, and nginx,
# still sending the body, then took the device for offline: 503)
cat > "$BUILD_CTX/web.py" <<'PY'
import http.server, sys
class H(http.server.SimpleHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0)); got = 0
        while got < n:
            d = self.rfile.read(min(65536, n - got))
            if not d:
                break
            got += len(d)
        body = b"got %d\n" % got
        self.send_response(200); self.send_header("Content-Length", str(len(body)))
        self.end_headers(); self.wfile.write(body)
http.server.ThreadingHTTPServer(("", int(sys.argv[1])), H).serve_forever()
PY
cat > "$BUILD_CTX/Dockerfile" <<'DOCKERFILE'
FROM ubuntu:24.04
ENV CAMEX_POOL_HOLD=0
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
        nginx curl python3 iputils-ping iproute2 openssl >/dev/null && \
    rm -rf /var/lib/apt/lists/* /etc/nginx/sites-enabled/default
COPY camex /usr/local/bin/camex
COPY web.py /usr/local/bin/web.py
COPY server.key /etc/camex/server.key
COPY camex.conf camex2.conf /etc/camex/
COPY nginx-camex.conf /etc/nginx/conf.d/nginx-camex.conf
COPY page-pw /etc/camex/nginx-password
RUN printf 'camex:%s\n' "$(openssl passwd -apr1 -stdin < /etc/camex/nginx-password)" \
        > /etc/nginx/camex.htpasswd
RUN printf '#!/bin/sh\nmkdir -p /dev/net\n[ -e /dev/net/tun ] || mknod /dev/net/tun c 10 200\nexec /usr/local/bin/camex "$@"\n' \
        > /usr/local/bin/camex-run && chmod +x /usr/local/bin/camex-run
ENTRYPOINT ["/usr/local/bin/camex-run"]
DOCKERFILE
docker build -q -t "$IMAGE" "$BUILD_CTX" >/dev/null || { echo "cannot build $IMAGE"; exit 1; }
docker network create "$NET_NAME" >/dev/null || { echo "cannot create $NET_NAME"; exit 1; }
set +e   # checks below report failures themselves

client() {  # client <name> [<server port> [<web port>]]: "I am <name>" on its web port
    local web=${3:-80} extra=
    [ "$web" != 80 ] && extra="--web-port $web"
    docker run -d --name "$P-$1" --network "$NET_NAME" --cap-add NET_ADMIN \
        --cap-add MKNOD -e CAMEX_ID="$1" -e WEB="$web" --entrypoint /bin/sh "$IMAGE" -c \
        'mkdir -p /www && echo "I am $CAMEX_ID" > /www/index.html &&
         (cd /www && python3 /usr/local/bin/web.py "$WEB" >/dev/null 2>&1 &) &&
         exec /usr/local/bin/camex-run --mode client --auto --name "$CAMEX_ID" \
             --server-host '"$P-srv"' --port '"${2:-5800}"' --server-pubkey '"$SERVER_PUBKEY"' \
             --psk '"$PSK"' --keepalive 2 '"$extra" >/dev/null
}

client_quiet() {  # client_quiet <name> <server port> <web port>: serves there,
                  # announces nothing (the server's device_port= applies)
    docker run -d --name "$P-$1" --network "$NET_NAME" --cap-add NET_ADMIN \
        --cap-add MKNOD -e CAMEX_ID="$1" -e WEB="$3" --entrypoint /bin/sh "$IMAGE" -c \
        'mkdir -p /www && echo "I am $CAMEX_ID" > /www/index.html &&
         (cd /www && python3 /usr/local/bin/web.py "$WEB" >/dev/null 2>&1 &) &&
         exec /usr/local/bin/camex-run --mode client --auto --name "$CAMEX_ID" \
             --server-host '"$P-srv"' --port '"$2"' --server-pubkey '"$SERVER_PUBKEY"' \
             --psk '"$PSK"' --keepalive 2' >/dev/null
}

get() {     # get <host> [path]: what nginx serves for that name
    docker exec "$P-srv" curl -s -m 10 -u "camex:$PAGE_PW" -H "Host: $1" "http://127.0.0.1${2:-/}"
}

code() {    # code <host> [path] [curl args]: nginx's status for that name
    local h=$1 p=${2:-/}
    shift 2 2>/dev/null || shift
    docker exec "$P-srv" curl -s -o /dev/null -w '%{http_code}' -m 10 "$@" -H "Host: $h" "http://127.0.0.1$p"
}

wait_registered() {  # wait_registered <container> <id>...
    local c=$1 id _
    shift
    for id in "$@"; do
        for _ in $(seq 1 20); do
            docker logs "$c" 2>&1 | grep -q "Registered auto client $id" && break
            sleep 1
        done
    done
}

echo "== 1. two clients behind nginx"
docker run -d --name "$P-srv" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    -e CAMEX_CLIENT_TIMEOUT=5 "$IMAGE" --mode server --port 5800 --transport udp \
    --local-cidr 10.200.0.1/24 $SERVER_KEY_ARGS --psk "$PSK" \
    --config /etc/camex/camex.conf >/dev/null
sleep 1
docker logs "$P-srv" 2>&1 | grep -q "Start page: http://127.0.0.1:5801/"
check $? "the server says where its page is"
docker exec "$P-srv" nginx -t >/dev/null 2>&1
check $? "nginx -t accepts examples/nginx-camex.conf"
docker exec "$P-srv" nginx
NGINX_PID=$(docker exec "$P-srv" cat /run/nginx.pid)
client CAM-A
client CAM-B
wait_registered "$P-srv" CAM-A CAM-B
[ "$(get cam-a.$DOMAIN)" = "I am CAM-A" ] && [ "$(get cam-b.$DOMAIN)" = "I am CAM-B" ]
check $? "each name gets its own client's page, without a map"
[ "$(get CAM-A.$DOMAIN:80)" = "I am CAM-A" ]
check $? "the name in another case and with nginx's port"
get nobody.$DOMAIN | grep -q "Device not reachable" &&
    [ "$(code nobody.$DOMAIN)" = 503 ]
check $? "an unknown name gets the offline page (503)"

echo "== 2. a client's own web port"
client CAM-W 5800 8080
wait_registered "$P-srv" CAM-W
[ "$(get cam-w.$DOMAIN)" = "I am CAM-W" ]
check $? "a client with --web-port 8080 is reached on 8080"

echo "== 3. the start page through nginx"
[ "$(code "$DOMAIN" /)" = 401 ]
check $? "it asks for nginx's password"
home=$(get "$DOMAIN")
echo "$home" | grep -qF "<a href=\"//cam-a.$DOMAIN/\">cam-a</a>" &&
    echo "$home" | grep -qF "<a href=\"//cam-w.$DOMAIN/\">cam-w</a>" &&
    echo "$home" | grep -q '<span class="nm">Gate</span>'
check $? "it lists the clients by name, links by name, and the configured CAM-Z"
echo "$home" | grep -q '<meta name="camex-version"'
check $? "it is camex's own page"
[ "$(code "$DOMAIN:8000" / -u "camex:$PAGE_PW")" = 200 ] &&
    docker exec "$P-srv" curl -s -m 10 -u "camex:$PAGE_PW" -H "Host: $DOMAIN:8000" http://127.0.0.1/ |
        grep -qF "<a href=\"http://cam-a.$DOMAIN:8000/\">"
check $? "links carry nginx's port when it is not 80"
docker exec "$P-srv" curl -s -m 10 -u "camex:$PAGE_PW" -H "Host: $DOMAIN" \
        -H "X-Forwarded-Proto: https" http://127.0.0.1/ |
    grep -qF "<a href=\"//cam-a.$DOMAIN/\">"
check $? "over HTTPS the links have no scheme or port"
[ "$(code "$DOMAIN" /resolve -u "camex:$PAGE_PW")" = 404 ]
check $? "/resolve is not reachable through the start page"
st=$(docker exec "$P-srv" curl -s -o /dev/null -w '%{http_code}' -m 5 -H "X-Camex-Host: cam-a.$DOMAIN" \
     -H "X-Forwarded-Host: x" http://127.0.0.1:5801/resolve)
[ "$st" = 404 ]
check $? "/resolve refuses a request passed on by a page proxy (X-Forwarded-Host)"
hdr=$(docker exec "$P-srv" curl -s -D - -o /dev/null -m 5 -H "X-Camex-Host: cam-a.$DOMAIN" \
      http://127.0.0.1:5801/resolve)
echo "$hdr" | grep -q '^HTTP/1.0 204' && echo "$hdr" | grep -Eq '^X-Camex-Upstream: 10\.200\.0\.[0-9]+:80'
check $? "/resolve on 127.0.0.1: 204 and X-Camex-Upstream"
[ "$(docker exec "$P-srv" curl -s -o /dev/null -w '%{http_code}' -m 5 -H "X-Camex-Host: nobody.$DOMAIN" \
     http://127.0.0.1:5801/resolve)" = 403 ]
check $? "/resolve of an unknown name: 403"
SRV_IP=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$P-srv")
! docker exec "$P-CAM-A" curl -s -m 3 -o /dev/null "http://$SRV_IP:5801/" 2>/dev/null
check $? "web_listen = yes is not reachable from another host"

echo "== 4. a client goes away"
docker rm -f "$P-CAM-B" >/dev/null
gone=1
for _ in $(seq 1 20); do
    [ "$(code cam-b.$DOMAIN)" = 503 ] && { gone=0; break; }
    sleep 1
done
check $gone "its name gets the offline page once the server drops it"
[ "$(docker exec "$P-srv" cat /run/nginx.pid)" = "$NGINX_PID" ] &&
    ! docker exec "$P-srv" grep -q "reconfiguring" /var/log/nginx/error.log
check $? "nginx was never reloaded or restarted"

echo "== 5. a second server on the same host"
docker exec -d "$P-srv" /usr/local/bin/camex-run --mode server --port 5900 --transport udp \
    --local-cidr 10.201.0.1/24 $SERVER_KEY_ARGS --psk "$PSK" --config /etc/camex/camex2.conf
sleep 1
client_quiet CAM-C 5900 8090
for _ in $(seq 1 20); do
    docker exec "$P-srv" camex --status 2>/dev/null | grep -q CAM-C && break
    sleep 1
done
sleep 3   # the page asks the others every 2 s
[ "$(get cam-c.$DOMAIN)" = "I am CAM-C" ]
check $? "a client of the other server is reached by name, on that server's device_port 8090"
get "$DOMAIN" | grep -qF "<a href=\"//cam-c.$DOMAIN/\">cam-c</a>"
check $? "and is on the start page"

echo "== 6. /status.json"
json=$(get "$DOMAIN" /status.json)
n=$(printf '%s' "$json" | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["servers"]))' 2>/dev/null)
[ "$n" = 2 ]
check $? "it is JSON with both servers (${n:-not JSON})"

echo "   the other server killed (its socket left behind)"
docker exec "$P-srv" sh -c 'for d in /proc/[0-9]*; do
        tr "\0" " " < $d/cmdline 2>/dev/null | grep -q "camex --mode server --port 590[0]" &&
            kill -9 "${d#/proc/}"
    done' 2>/dev/null
gone=1
for _ in $(seq 1 25); do
    ! get "$DOMAIN" | grep -q "cam-c" && [ "$(code cam-c.$DOMAIN)" = 503 ] && { gone=0; break; }
    sleep 1
done
check $gone "its clients leave the page and nginx's lookup"

echo "   web_scope = self"
docker exec "$P-srv" sed -i '/^\[server\]$/a web_scope=self' /etc/camex/camex.conf
docker exec "$P-srv" kill -HUP 1   # the first server is the container's process
sleep 2
! get "$DOMAIN" | grep -q "cam-c" && [ "$(code cam-c.$DOMAIN)" = 503 ]
check $? "only its own clients: CAM-C is not on the page or reached"

echo "== 7. the page on the LAN without nginx"
printf '[server]\nweb_listen=0.0.0.0:5801\n' > "$BUILD_CTX/lan-bad.conf"
lan_server() {   # lan_server <name> <conf>: a server of that file, no nginx
    docker create --name "$P-$1" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
        "$IMAGE" --mode server --port 5800 --local-cidr 10.200.0.1/24 \
        $SERVER_KEY_ARGS --psk "$PSK" --config /etc/camex/lan.conf >/dev/null
    docker cp "$2" "$P-$1:/etc/camex/lan.conf" >/dev/null
    docker start "$P-$1" >/dev/null
}
lan_server lanbad "$BUILD_CTX/lan-bad.conf"
sleep 2
[ "$(docker inspect -f '{{.State.Running}}' "$P-lanbad")" = false ] &&
    docker logs "$P-lanbad" 2>&1 | grep -q "needs web_password="
check $? "camex does not start with a LAN page and no web_password="
printf '[server]\nweb_listen=0.0.0.0:5801\nweb_password=%s\n' "$LAN_HASH" > "$BUILD_CTX/lan.conf"
lan_server lan "$BUILD_CTX/lan.conf"
sleep 1
docker run -d --name "$P-cam-l" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    -e CAMEX_ID=CAM-L --entrypoint /bin/sh "$IMAGE" -c \
    'mkdir -p /www && echo "I am CAM-L" > /www/index.html &&
     (cd /www && python3 -m http.server 8081 >/dev/null 2>&1 &) &&
     exec /usr/local/bin/camex-run --mode client --auto --name CAM-L \
         --server-host '"$P-lan"' --port 5800 --server-pubkey '"$SERVER_PUBKEY"' \
         --psk '"$PSK"' --web-port 8081' >/dev/null
wait_registered "$P-lan" CAM-L
LAN_IP=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$P-lan")
lan() {     # lan [curl args]: the LAN page from another host
    docker exec "$P-CAM-A" curl -s -m 10 "$@" "http://$LAN_IP:5801/"
}
[ "$(lan -o /dev/null -w '%{http_code}')" = 401 ] &&
    lan -D - -o /dev/null | grep -qi '^WWW-Authenticate: Basic'
check $? "without the password: 401 and a Basic challenge"
page=$(lan -u "any:$LAN_PW")
echo "$page" | grep -Eq '<a class="peer" href="http://10\.200\.0\.[0-9]+:8081/">cam-l</a>'
check $? "with it: the page, linking the device's tunnel address and web port"
[ "$(lan -u "any:$LAN_PW" -o /dev/null -w '%{http_code}')" = 200 ]
check $? "the same password again (remembered, no argon2): 200"
for _ in 1 2 3 4 5; do lan -u "any:wrong" -o /dev/null; done
[ "$(lan -u "any:$LAN_PW" -o /dev/null -w '%{http_code}')" = 429 ]
check $? "after 5 wrong passwords that address gets 429, even with the right one"
docker logs "$P-lan" 2>&1 | grep -q "wrong passwords from"
check $? "the server logs it"

echo "   16 idle connections from the network do not keep a request out"
# (from CAM-W: CAM-A is shut out for a minute by the step above)
docker exec -d "$P-CAM-W" python3 -c '
import socket, time
s = [socket.create_connection(("'"$LAN_IP"'", 5801), 3) for _ in range(16)]
time.sleep(8)'
sleep 1
[ "$(docker exec "$P-CAM-W" curl -s -m 10 -u "any:$LAN_PW" -o /dev/null -w '%{http_code}' "http://$LAN_IP:5801/")" = 200 ]
check $? "a request gets its answer while 16 idle ones hold the slots"

echo "   a reload that drops web_password= closes the LAN page"
printf '[server]\nweb_listen=0.0.0.0:5801\n' > "$BUILD_CTX/lan-nopw.conf"
docker cp "$BUILD_CTX/lan-nopw.conf" "$P-lan:/etc/camex/lan.conf" >/dev/null
docker exec "$P-lan" kill -HUP 1
sleep 2
! lan -o /dev/null && docker logs "$P-lan" 2>&1 | grep -q "web_password= is gone"
check $? "the page no longer answers, and the log says why"

echo "== 8. what it refuses"
raw() {     # raw <request>: the status line camex answers on 127.0.0.1:5801
    docker exec "$P-srv" python3 -c '
import socket, sys
s = socket.create_connection(("127.0.0.1", 5801), 5)
s.sendall(sys.argv[1].encode())
print(s.recv(200).decode(errors="replace").split("\r\n")[0])' "$1"
}
raw $'POST / HTTP/1.0\r\n\r\n' | grep -q '^HTTP/1.0 405'
check $? "POST: 405"
raw "GET / HTTP/1.0"$'\r\n'"X-Big: $(head -c 9000 /dev/zero | tr '\0' a)"$'\r\n\r\n' | grep -q '^HTTP/1.0 431'
check $? "a head over 8 KB: 431"
raw $'GET /etc/passwd HTTP/1.0\r\n\r\n' | grep -q '^HTTP/1.0 404'
check $? "another path: 404"
raw $'garbage\r\n\r\n' | grep -q '^HTTP/1.0 400'
check $? "not HTTP: 400"
docker exec "$P-srv" camex --status 2>/dev/null | grep -q CAM-A
check $? "the server still runs and serves its clients"

echo "== 9. uploads above 1 MB; the page refreshes itself in place (#142)"
client CAM-U 5800 8080
wait_registered "$P-srv" CAM-U
# 2 MB through nginx to a device: nginx passes all of it on (the device
# answers with what it got), no 413 of its 1 MB default
got=$(docker exec "$P-srv" sh -c 'head -c 2097152 /dev/zero > /tmp/big &&
    curl -s -w " %{http_code}" -m 20 -H "Host: cam-u.'"$DOMAIN"'" \
        --data-binary @/tmp/big http://127.0.0.1/' | tr -d '\n')
[ "$got" = "got 2097152 200" ]
check $? "a 2 MB upload goes through nginx to the device (got: ${got:-nothing}; want: got 2097152 200)"
# An nginx file of 5.2 asks /resolve with the request's own method
st=$(docker exec "$P-srv" curl -s -o /dev/null -w '%{http_code}' -m 5 -X POST \
     -H "X-Camex-Host: cam-u.$DOMAIN" http://127.0.0.1:5801/resolve)
[ "$st" = 204 ]
check $? "/resolve answers a POST as a GET (an nginx file without proxy_method GET), got ${st:-nothing}"
# The page as camex serves it, a next one (other counts, a marked list) and
# one of another camex version; its script runs in jsdom with fetch
# answering those
JS_DIR="$BUILD_CTX/start-page"
mkdir -p "$JS_DIR"
cp "$REPO_DIR/tests/start-page/refresh-test.js" "$JS_DIR/"
get "$DOMAIN" > "$JS_DIR/a.html"
grep -q '<noscript><meta http-equiv="refresh" content="30"></noscript>' "$JS_DIR/a.html" &&
    grep -q '<meta name="camex-version" content="' "$JS_DIR/a.html"
check $? "the page has its version, and the 30-second reload only without JS"
sed -e 's|<div class="tabs">|<div class="tabs" data-t="new">|' \
    -e 's|<div class="list list-conf">|<div class="list list-conf" data-l="new">|' \
    -e 's|<div class="list list-auto">|<div class="list list-auto" data-l="new">|' \
    -e 's|<div class="sub">[0-9]* online|<div class="sub">99 online|' \
    -e 's|<title>[0-9]* online|<title>98 online|' \
    -e '/^<link rel="icon"/s|fill=%27%23[0-9a-f]*%27|fill=%27%23123456%27|' \
    "$JS_DIR/a.html" > "$JS_DIR/b.html"
sed 's|<meta name="camex-version" content="[^"]*"|<meta name="camex-version" content="0.0.0"|' \
    "$JS_DIR/b.html" > "$JS_DIR/c.html"
# (the files go in through stdin: a temporary directory need not be one
# the Docker daemon sees; a failure, npm unreachable too, is the check's)
rc=0
out=$(tar -C "$JS_DIR" -cf - . | docker run --rm -i -e HOME=/tmp -e npm_config_cache=/tmp/npm \
    node:24-slim sh -c 'mkdir -p /t && cd /t && tar -xf - &&
        { npm install --silent --no-audit --no-fund jsdom@24 >/dev/null 2>&1 ||
          { echo "FAIL: npm install jsdom@24"; exit 1; }; } &&
        node refresh-test.js' 2>&1) || rc=$?
echo "$out" | sed 's/^/      /'
[ $rc -eq 0 ] && ! echo "$out" | grep -q "^FAIL"
check $? "the page's script: swaps the lists in place, keeps the tab, marks no connection, reloads for a new version"

echo
if [ "$FAILS" != 0 ]; then
    echo "--- server log (tail) ---"
    docker logs "$P-srv" 2>&1 | tail -15
    echo "FAIL: $FAILS check(s) failed"
    exit 2
fi
echo "PASS: camex serves the start page and nginx's lookup itself"
