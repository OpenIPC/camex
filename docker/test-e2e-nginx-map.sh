#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-e2e-nginx-map.sh — nginx in front of camex clients by name (#84)
#
# The server and nginx in one container, with examples/nginx-camex.conf
# and examples/camex-nginx-sync.sh; each client serves a page with its ID
# on port 80 of its tunnel address.
#
# 1. Two clients: the map has both, nginx -t accepts it, each name gets its
#    own client's page; an unknown name gets the offline page.
# 2. A sync without a change does not reload nginx.
# 3. A client with an ID that is no host name is left out, with a warning.
# 4. A client that goes away is gone from the map after the next sync, and
#    its name gets the offline page.
# 5. A socket left behind by a killed server does not stop the sync.
# 6. A reload that failed (nginx not running) is tried again by the next
#    sync, although the map did not change; the start page waits for it.
# The start page (#105) along the way: the bare domain lists the live
# clients with their name= (escaped) or ID and links that reach them;
# --status shows the name; an unchanged state leaves the file alone; bad
# IDs and clients that are gone are not on it.
#
# Usage: ./test-e2e-nginx-map.sh
#   CAMEX_BINARY=path/to/camex  binary to test (default: ../camex)
#   KEEP=1                      keep containers for debugging
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/camex}"
IMAGE="camex:e2e-nginx"
P="camex-nginx-$$"
NET_NAME="$P-net"
BUILD_CTX="$(mktemp -d)"
PSK="nginx-test"
DOMAIN="cam.test"
INDEX="/var/lib/camex-nginx/index.html"
# CAM-A's name= has HTML characters and Cyrillic
NAME_A='Склад <1> & "A"'
NAME_A_HTML='Склад &lt;1&gt; &amp; &quot;A&quot;'
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
cp "$REPO_DIR/examples/camex-nginx-sync.sh" "$BUILD_CTX/"
sed "s/cam\.example\.lan/$DOMAIN/g" "$REPO_DIR/examples/nginx-camex.conf" > "$BUILD_CTX/nginx-camex.conf"
. "$SCRIPT_DIR/lib-keys.sh"
gen_server_key "$BUILD_CTX" "$CAMEX_BINARY" || { echo "cannot create a server key"; exit 1; }
# CAM-Z is configured but never connects: offline on the start page
printf '[client CAM-A]\nname=%s\n[client CAM-Z]\nname=Gate\n' "$NAME_A" > "$BUILD_CTX/camex.conf"
cat > "$BUILD_CTX/Dockerfile" <<'DOCKERFILE'
FROM ubuntu:24.04
# New clients right after the server starts: no start hold (#81)
ENV CAMEX_POOL_HOLD=0
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
        nginx-light curl python3 iputils-ping iproute2 >/dev/null && \
    rm -rf /var/lib/apt/lists/* /etc/nginx/sites-enabled/default
COPY camex /usr/local/bin/camex
COPY server.key /etc/camex/server.key
COPY camex.conf /etc/camex/camex.conf
RUN mkdir -p /var/lib/camex-nginx
COPY camex-nginx-sync.sh /usr/local/bin/camex-nginx-sync.sh
COPY nginx-camex.conf /etc/nginx/conf.d/nginx-camex.conf
# Counts nginx reloads for the test
RUN printf '#!/bin/sh\n[ "$1" = "-s" ] && echo reload >> /tmp/reloads\nexec /usr/sbin/nginx "$@"\n' \
        > /usr/local/bin/nginx-counting && chmod +x /usr/local/bin/nginx-counting
RUN printf '#!/bin/sh\nmkdir -p /dev/net\n[ -e /dev/net/tun ] || mknod /dev/net/tun c 10 200\nexec /usr/local/bin/camex "$@"\n' \
        > /usr/local/bin/camex-run && chmod +x /usr/local/bin/camex-run
ENTRYPOINT ["/usr/local/bin/camex-run"]
DOCKERFILE
docker build -q -t "$IMAGE" "$BUILD_CTX" >/dev/null 2>&1
docker network create "$NET_NAME" >/dev/null
set +e   # checks below report failures themselves

client() {  # client <name>: a camex client serving "I am <name>" on port 80
    docker run -d --name "$P-$1" --network "$NET_NAME" --cap-add NET_ADMIN \
        --cap-add MKNOD -e CAMEX_ID="$1" --entrypoint /bin/sh "$IMAGE" -c \
        'mkdir -p /www && echo "I am $CAMEX_ID" > /www/index.html &&
         (cd /www && python3 -m http.server 80 >/dev/null 2>&1 &) &&
         exec /usr/local/bin/camex-run --mode client --auto --name "$CAMEX_ID" \
             --server-host '"$P-srv"' --port 5800 --server-pubkey '"$SERVER_PUBKEY"' \
             --psk '"$PSK"' --keepalive 2' >/dev/null
}

sync() {    # sync: the example sync script in the server container; nginx
            # reloads in the background, so give its new workers a moment
    docker exec -e DOMAIN="$DOMAIN" -e NGINX_BIN=nginx-counting \
        -e INDEX="$INDEX" "$P-srv" camex-nginx-sync.sh 2>&1
    sleep 1
}

get() {     # get <host>: what nginx serves for that name
    docker exec "$P-srv" curl -s -m 10 -H "Host: $1" http://127.0.0.1/
}

reloads() {
    docker exec "$P-srv" sh -c 'wc -l < /tmp/reloads 2>/dev/null || echo 0'
}

wait_registered() {  # wait_registered <id>...
    local id _
    for id in "$@"; do
        for _ in $(seq 1 20); do
            docker logs "$P-srv" 2>&1 | grep -q "Registered auto client $id" && break
            sleep 1
        done
    done
}

echo "== 1. two clients behind nginx"
docker run -d --name "$P-srv" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    -e CAMEX_CLIENT_TIMEOUT=5 "$IMAGE" --mode server --port 5800 \
    --local-cidr 10.200.0.1/24 $SERVER_KEY_ARGS --psk "$PSK" \
    --config /etc/camex/camex.conf >/dev/null
sleep 1
# An empty map so that nginx starts
docker exec "$P-srv" sh -c 'printf "map \$host \$camex_upstream {\n}\n" > /etc/nginx/camex-map.conf && nginx'
client CAM-A
client CAM-B
wait_registered CAM-A CAM-B
sleep 2
out=$(sync)
echo "$out" | sed 's/^/      /'
docker exec "$P-srv" cat /etc/nginx/camex-map.conf | sed 's/^/      /'
docker exec "$P-srv" grep -Eq "^    \"cam-a\.$DOMAIN\" 10\.200\.0\.[0-9]+:80;$" /etc/nginx/camex-map.conf &&
    docker exec "$P-srv" grep -Eq "^    \"cam-b\.$DOMAIN\" 10\.200\.0\.[0-9]+:80;$" /etc/nginx/camex-map.conf
check $? "the map has both clients"
docker exec "$P-srv" nginx -t >/dev/null 2>&1
check $? "nginx -t accepts the configuration"
[ "$(get cam-a.$DOMAIN)" = "I am CAM-A" ] && [ "$(get cam-b.$DOMAIN)" = "I am CAM-B" ]
check $? "each name gets its own client's page"
get nobody.$DOMAIN | grep -q "is offline"
check $? "an unknown name gets the offline page"
! get "<b>x.$DOMAIN" | grep -q "<b>"
check $? "the offline page does not echo the Host header"
echo "$out" | grep -q "start page updated: 2 devices online"
check $? "the sync writes the start page"
home=$(get "$DOMAIN")
echo "$home" | grep '^<tr class="dev">' | sed 's/^/      /'
echo "$home" | grep -qF "<a href=\"//cam-a.$DOMAIN/\">$NAME_A_HTML</a></td><td class=\"id\">cam-a</td>" &&
    echo "$home" | grep -qF "<a href=\"//cam-b.$DOMAIN/\">cam-b</a></td><td class=\"id\">cam-b</td>" &&
    echo "$home" | grep -qF '<div class="sub">2 online &middot; 2 configured</div>'
check $? "the bare domain lists both, CAM-A by its escaped name, CAM-B by its ID"
conf_part=$(echo "$home" | sed -n '/<div class="list list-conf">/,/<div class="list list-auto">/p')
auto_part=$(echo "$home" | sed -n '/<div class="list list-auto">/,$p')
echo "$conf_part" | grep -q "//cam-a.$DOMAIN/" && ! echo "$conf_part" | grep -q "//cam-b." &&
    echo "$auto_part" | grep -q "//cam-b.$DOMAIN/" && ! echo "$auto_part" | grep -q "//cam-a."
check $? "CAM-A (configured) under Configured, CAM-B (auto pool) under Auto"
echo "$conf_part" | grep -qF '<tr class="dev off"><td class="lamp"><span class="bulb"></span></td><td><span class="nm">Gate</span></td><td class="id">cam-z</td>' &&
    ! echo "$home" | grep -q "//cam-z." &&
    echo "$home" | grep -qF 'Configured <b>2</b> &middot; <i>1 offline</i>'
check $? "CAM-Z, configured and never connected, is listed offline without a link"
hs=$(docker exec "$P-srv" camex --status --json | python3 -c 'import json,sys; print([c for s in json.load(sys.stdin)["servers"] for c in s["clients"] if c["id"]=="CAM-A"][0]["handshakes"])')
echo "$home" | grep -F "//cam-a.$DOMAIN/" | grep -qE "<td class=\"hs\">[0-9dhms]+ ago &middot; $hs</td><td class=\"tr\">[0-9.]+ / [0-9.]+</td>"
check $? "CAM-A's row has its handshakes ($hs, as --json says), ages and traffic"
ok=0
for host in $(echo "$home" | sed -n 's|.*<a href="//\([^/"]*\)/">.*|\1|p'); do
    case "$(get "$host")" in "I am "*) ;; *) ok=1 ;; esac
done
check $ok "each link on the page reaches its client"
docker exec "$P-srv" curl -s -m 10 -H "Host: $DOMAIN" http://127.0.0.1/other | grep -q "404"
check $? "the bare domain serves nothing else"
docker exec "$P-srv" camex --status --json | grep -qF '"id":"CAM-A","name":"Склад <1> & \"A\""' &&
    docker exec "$P-srv" camex --status --json | grep -qF '"id":"CAM-B","name":null' &&
    docker exec "$P-srv" camex --status | grep -E '^CAM-A ' | grep -qF "  $NAME_A"
check $? "--status and --json show the name"

echo "== 2. a sync without a change"
before=$(reloads)
rx_before=$(get "$DOMAIN" | grep -F "//cam-a.$DOMAIN/" | sed -n 's|.*<td class="tr">\([0-9.]*\) / .*|\1|p')
addr_a=$(docker exec "$P-srv" camex --status --json | python3 -c 'import json,sys; print([c for s in json.load(sys.stdin)["servers"] for c in s["clients"] if c["id"]=="CAM-A"][0]["address"])')
docker exec "$P-srv" ping -c 20 -i 0.2 -s 1000 "$addr_a" >/dev/null 2>&1
sleep 1
out=$(sync)
[ "$(reloads)" = "$before" ]
check $? "nginx is not reloaded ($before -> $(reloads))"
! echo "$out" | grep -q "start page updated"
check $? "the same devices online: the page's new values are not logged"
rx_after=$(get "$DOMAIN" | grep -F "//cam-a.$DOMAIN/" | sed -n 's|.*<td class="tr">\([0-9.]*\) / .*|\1|p')
python3 -c "import sys; sys.exit(0 if float('${rx_after:-0}') > float('${rx_before:-0}') else 1)"
check $? "CAM-A's traffic on the page grows with pings ($rx_before -> $rx_after KiB)"
out=$(docker exec -e DOMAIN="$DOMAIN" -e NGINX_BIN=nginx-counting \
    -e INDEX=/nonexistent/index.html "$P-srv" camex-nginx-sync.sh 2>&1)
echo "$out" | grep -q "cannot write /nonexistent/index.html.tmp.* (does the directory of INDEX exist?)" &&
    ! echo "$out" | grep -q "did not answer"
check $? "a missing page directory is named, not blamed on the server"

echo "== 3. an ID that is no host name"
client bad_id
wait_registered bad_id
out=$(sync)
echo "$out" | grep -q "client ID bad_id is not a host name label: left out of the map" &&
    ! docker exec "$P-srv" grep -q "bad" /etc/nginx/camex-map.conf
check $? "it is left out with a warning"
echo "$out" | grep -q "client ID bad_id is not a host name label: left out of the start page" &&
    ! get "$DOMAIN" | grep -qi "bad"
check $? "and left out of the start page"

echo "== 4. a client goes away"
docker rm -f "$P-CAM-B" >/dev/null
gone=1
for _ in $(seq 1 20); do
    docker logs "$P-srv" 2>&1 | grep -q "Expired client" && { gone=0; break; }
    sleep 1
done
check $gone "the server expires it"
sync >/dev/null
! docker exec "$P-srv" grep -q "cam-b" /etc/nginx/camex-map.conf
check $? "it is gone from the map"
get cam-b.$DOMAIN | grep -q "is offline"
check $? "its name gets the offline page"
! get "$DOMAIN" | grep -q "cam-b" && get "$DOMAIN" | grep -qF "//cam-a.$DOMAIN/"
check $? "it is gone from the start page"
a=$(get cam-a.$DOMAIN)
[ "$a" = "I am CAM-A" ]
check $? "the other one still works (${a:-no answer})"

echo "== 5. a socket left behind by a server that is gone"
# A second server killed hard leaves its socket in /run/camex
docker exec -d "$P-srv" camex --mode server --port 5899 --local-cidr 10.201.0.1/24 \
    $SERVER_KEY_ARGS --psk "$PSK"
sleep 1
docker exec "$P-srv" sh -c 'pkill -9 -f "^camex --mode server --port 5899"; sleep 1; test -S /run/camex/camex-udp-5899.sock'
check $? "the killed server's socket is still there"
out=$(sync)
echo "$out" | grep -q "camex-udp-5899.sock: no server listens there, skipped" &&
    [ "$(get cam-a.$DOMAIN)" = "I am CAM-A" ]
check $? "the sync skips it and the map still follows the live server"

echo "== 6. a reload that fails is tried again"
# nginx that cannot reload (not running yet): the map is written but not
# counted as applied, so the next run reloads although it did not change
docker exec "$P-srv" sh -c 'printf "#!/bin/sh\n[ \"\$1\" = \"-s\" ] && exit 1\nexec /usr/sbin/nginx \"\$@\"\n" > /usr/local/bin/nginx-down && chmod +x /usr/local/bin/nginx-down'
client CAM-C
wait_registered CAM-C
sleep 2
out=$(docker exec -e DOMAIN="$DOMAIN" -e NGINX_BIN=nginx-down -e INDEX="$INDEX" \
    "$P-srv" camex-nginx-sync.sh 2>&1)
echo "$out" | grep -q "nginx did not reload" &&
    docker exec "$P-srv" grep -q "cam-c" /etc/nginx/camex-map.conf
check $? "the failed reload is reported and the new map kept"
echo "$out" | grep -q "the map was not applied: $INDEX left as it is" &&
    ! docker exec "$P-srv" grep -q "cam-c" "$INDEX"
check $? "the start page waits for the map (no link nginx does not know yet)"
before=$(reloads)
sync >/dev/null
[ "$(reloads)" -gt "$before" ] && [ "$(get cam-c.$DOMAIN)" = "I am CAM-C" ]
check $? "the next run reloads nginx although the map did not change"
docker exec "$P-srv" grep -qF "//cam-c.$DOMAIN/" "$INDEX"
check $? "and the start page follows"

echo "== 7. one sync at a time"
# A run that holds the lock (a slow server, say): the next one waits for it
# instead of taking the lock by age, then does its work
client CAM-D
wait_registered CAM-D
sleep 2
docker exec -d "$P-srv" flock /etc/nginx/camex-map.conf.lock sleep 4
sleep 1
t0=$(date +%s)
sync >/dev/null
t1=$(date +%s)
[ $((t1 - t0)) -ge 3 ] && [ "$(get cam-d.$DOMAIN)" = "I am CAM-D" ]
check $? "the second run waited for the lock ($((t1 - t0)) s) and applied its map"

echo "== 8. IDs that are map keywords, no domain"
# Without a domain the keys are bare IDs: default, include, hostnames and
# volatile must stay hosts, not map's own parameters
for id in default include; do client "$id"; done
wait_registered default include
sleep 2
out=$(docker exec -e DOMAIN= -e NGINX_BIN=nginx-counting "$P-srv" camex-nginx-sync.sh 2>&1)
sleep 1
echo "$out" | sed 's/^/      /'
docker exec "$P-srv" grep -q '^    "\\default" ' /etc/nginx/camex-map.conf &&
    docker exec "$P-srv" nginx -t >/dev/null 2>&1
check $? "the map has them and nginx -t accepts it"
[ "$(get default)" = "I am default" ] && [ "$(get include)" = "I am include" ] &&
    get nobody | grep -q "is offline"
check $? "each one gets its own page, an unknown name still the offline page"

if [ "$FAILS" -ne 0 ]; then
    echo "FAIL: $FAILS check(s) failed"
    exit 1
fi
echo "PASS: nginx follows the camex clients by name"
