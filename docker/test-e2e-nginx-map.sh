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
# 11. The start page's script (#142), in jsdom: the lists swapped in place,
#    the tab kept, "no connection" on a failed fetch, a reload for another
#    camex version.
# 10. --follow (#130): a sync as soon as a client comes or goes, none while
#    nothing changes; a server too old for it is synced every 10 seconds; a
#    server that starts during the wait ends it (#138).
# The start page (#105) along the way: the bare domain lists the live
# clients with their name= (escaped) or ID and links that reach them;
# --status shows the name; an unchanged state leaves the file alone; bad
# IDs and clients that are gone are not on it. It asks for a password,
# and its Peer column links each client's external IP on its peer_port=
# (#119).
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
DOMAIN="openipc.click"
INDEX="/var/lib/camex-nginx/index.html"
# CAM-A's name= has HTML characters and Cyrillic
NAME_A='Склад <1> & "A"'
NAME_A_HTML='Склад &lt;1&gt; &amp; &quot;A&quot;'
# The start page's password of the hand-made setup (#119)
PAGE_PW="e2e-page-pw"
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
cp "$REPO_DIR/examples/camex-nginx-sync.sh" "$BUILD_CTX/"
sed "s/cam\.example\.lan/$DOMAIN/g" "$REPO_DIR/examples/nginx-camex.conf" > "$BUILD_CTX/nginx-camex.conf"
. "$SCRIPT_DIR/lib-keys.sh"
gen_server_key "$BUILD_CTX" "$CAMEX_BINARY" || { echo "cannot create a server key"; exit 1; }
# CAM-Z is configured but never connects: offline on the start page
# CAM-A's peer link on port 80, the others on the default 8080 (#119)
printf '[server]\ndomain=%s\n[client CAM-A]\nname=%s\npeer_port=80\n[client CAM-Z]\nname=Gate\n' \
    "$DOMAIN" "$NAME_A" > "$BUILD_CTX/camex.conf"
echo "$PAGE_PW" > "$BUILD_CTX/page-pw"
cat > "$BUILD_CTX/Dockerfile" <<'DOCKERFILE'
FROM ubuntu:24.04
# New clients right after the server starts: no start hold (#81)
ENV CAMEX_POOL_HOLD=0
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
        nginx-light curl python3 iputils-ping iproute2 openssl >/dev/null && \
    rm -rf /var/lib/apt/lists/* /etc/nginx/sites-enabled/default
COPY camex /usr/local/bin/camex
COPY server.key /etc/camex/server.key
COPY camex.conf /etc/camex/camex.conf
RUN mkdir -p /var/lib/camex-nginx
COPY camex-nginx-sync.sh /usr/local/bin/camex-nginx-sync
COPY nginx-camex.conf /etc/nginx/conf.d/nginx-camex.conf
COPY page-pw /etc/camex/nginx-password
RUN chmod 600 /etc/camex/nginx-password && \
    printf 'camex:%s\n' "$(openssl passwd -apr1 -stdin < /etc/camex/nginx-password)" \
        > /etc/nginx/camex.htpasswd
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

client() {  # client <name> [<port>]: a camex client serving "I am <name>" on port 80
    docker run -d --name "$P-$1" --network "$NET_NAME" --cap-add NET_ADMIN \
        --cap-add MKNOD -e CAMEX_ID="$1" --entrypoint /bin/sh "$IMAGE" -c \
        'mkdir -p /www && echo "I am $CAMEX_ID" > /www/index.html &&
         (cd /www && python3 -m http.server 80 >/dev/null 2>&1 &) &&
         exec /usr/local/bin/camex-run --mode client --auto --name "$CAMEX_ID" \
             --server-host '"$P-srv"' --port '"${2:-5800}"' --server-pubkey '"$SERVER_PUBKEY"' \
             --psk '"$PSK"' --keepalive 2' >/dev/null
}

sync() {    # sync: the example sync script in the server container; nginx
            # reloads in the background, so give its new workers a moment
    docker exec -e NGINX_BIN=nginx-counting -e INDEX="$INDEX" "$P-srv" \
        camex-nginx-sync 2>&1
    sleep 1
}

get() {     # get <host>: what nginx serves for that name (the start
            # page's password along: the cameras ignore it)
    docker exec "$P-srv" curl -s -m 10 -u "camex:$PAGE_PW" -H "Host: $1" http://127.0.0.1/
}

ip_of() {   # ip_of <container>: its address, the peer the server sees
    docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$1"
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
echo "$home" | grep -F "//cam-a.$DOMAIN/" | grep -qE "<td class=\"hs\">[0-9dhms]+ ago &middot; $hs</td><td class=\"tr\">[0-9.&#;]+ / [0-9.&#;]+</td>"
check $? "CAM-A's row has its handshakes ($hs, as --json says), ages and traffic"
ok=0
for host in $(echo "$home" | sed -n 's|.*<a href="//\([^/"]*\)/">.*|\1|p'); do
    case "$(get "$host")" in "I am "*) ;; *) ok=1 ;; esac
done
check $ok "each link on the page reaches its client"
docker exec "$P-srv" curl -s -m 10 -u "camex:$PAGE_PW" -H "Host: $DOMAIN" http://127.0.0.1/other | grep -q "404"
check $? "the bare domain serves nothing else"
code=$(docker exec "$P-srv" curl -s -o /dev/null -w '%{http_code}' -m 10 -H "Host: $DOMAIN" http://127.0.0.1/)
code2=$(docker exec "$P-srv" curl -s -o /dev/null -w '%{http_code}' -m 10 -u camex:wrong -H "Host: $DOMAIN" http://127.0.0.1/)
[ "$code" = 401 ] && [ "$code2" = 401 ] && [ "$(get cam-a.$DOMAIN)" = "I am CAM-A" ]
check $? "the start page asks for the password ($code, a wrong one $code2); the cameras do not"
ip_a=$(ip_of "$P-CAM-A")
ip_b=$(ip_of "$P-CAM-B")
echo "$home" | grep -qF '<th class="addr">Address</th><th class="addr">Peer</th><th class="conn">'
check $? "the Peer column right after Address"
echo "$home" | grep -F "//cam-a.$DOMAIN/" | grep -qF "<td class=\"addr\"><a class=\"peer\" href=\"http://$ip_a/\" target=\"_blank\" rel=\"noopener\">$ip_a</a></td>" &&
    echo "$home" | grep -F "//cam-b.$DOMAIN/" | grep -qF "<td class=\"addr\"><a class=\"peer\" href=\"http://$ip_b:8080/\" target=\"_blank\" rel=\"noopener\">$ip_b</a></td>"
check $? "the peers: CAM-A $ip_a (peer_port=80, no port), CAM-B $ip_b:8080 (the default)"
echo "$home" | grep -F '<span class="nm">Gate</span>' | grep -qF '<td class="addr">&mdash;</td><td class="conn">offline'
check $? "CAM-Z, offline, has no peer"
[ "$(docker exec "$P-srv" curl -s -m 10 "http://$ip_a/")" = "I am CAM-A" ]
check $? "CAM-A's peer link reaches it"
docker exec "$P-srv" camex --status --json | grep -qF '"id":"CAM-A","name":"Склад <1> & \"A\""' &&
    docker exec "$P-srv" camex --status --json | grep -qF '"id":"CAM-B","name":null' &&
    docker exec "$P-srv" camex --status | grep -E '^CAM-A ' | grep -qF "  $NAME_A"
check $? "--status and --json show the name"

echo "== 2. a sync without a change"
before=$(reloads)
rx_of_a() {  # CAM-A's RX on the page, in KiB, without the thousands' spaces
    get "$DOMAIN" | grep -F "//cam-a.$DOMAIN/" | sed -e 's/&#8239;//g' |
        sed -n 's|.*<td class="tr">\([0-9.]*\) / .*|\1|p'
}
rx_before=$(rx_of_a)
addr_a=$(docker exec "$P-srv" camex --status --json | python3 -c 'import json,sys; print([c for s in json.load(sys.stdin)["servers"] for c in s["clients"] if c["id"]=="CAM-A"][0]["address"])')
docker exec "$P-srv" ping -c 20 -i 0.2 -s 1000 "$addr_a" >/dev/null 2>&1
sleep 1
out=$(sync)
[ "$(reloads)" = "$before" ]
check $? "nginx is not reloaded ($before -> $(reloads))"
! echo "$out" | grep -q "start page updated"
check $? "the same devices online: the page's new values are not logged"
rx_after=$(rx_of_a)
python3 -c "import sys; sys.exit(0 if float('${rx_after:-0}') > float('${rx_before:-0}') else 1)"
check $? "CAM-A's traffic on the page grows with pings ($rx_before -> $rx_after KiB)"
docker exec "$P-srv" chmod 600 "$INDEX"
sync >/dev/null
[ "$(docker exec "$P-srv" stat -c %a "$INDEX")" = 644 ]
check $? "a page with tight permissions is made readable for nginx"
out=$(docker exec -e NGINX_BIN=nginx-counting \
    -e INDEX=/nonexistent/index.html "$P-srv" camex-nginx-sync 2>&1)
echo "$out" | grep -q "cannot write /nonexistent/index.html.tmp.* (does the directory of INDEX exist?)" &&
    ! echo "$out" | grep -q "did not answer"
check $? "a missing page directory is named, not blamed on the server"
docker exec "$P-srv" sh -c 'camex --status --format nginx-index > /dev/full' 2>&1 | grep -q "cannot write the start page" &&
    ! docker exec "$P-srv" sh -c 'camex --status --format nginx-index > /dev/full' 2>/dev/null &&
    ! docker exec "$P-srv" sh -c 'camex --status --format nginx-map > /dev/full' 2>/dev/null
check $? "a write that fails (a full disk) is an error, not a cut-off page or map"

echo "== 2a. peer_port= follows a reload (#119)"
# none in [defaults]: CAM-B's IP without a link; CAM-A keeps its own 80
docker exec "$P-srv" sh -c 'printf "[defaults]\npeer_port=none\n" >> /etc/camex/camex.conf'
docker kill -s HUP "$P-srv" >/dev/null
sleep 1
sync >/dev/null
home=$(get "$DOMAIN")
echo "$home" | grep -F "//cam-b.$DOMAIN/" | grep -qF "<td class=\"addr\">$ip_b</td><td class=\"conn\">" &&
    echo "$home" | grep -F "//cam-a.$DOMAIN/" | grep -qF "href=\"http://$ip_a/\""
check $? "peer_port=none in [defaults]: CAM-B's IP is plain text, CAM-A keeps its link"
docker exec "$P-srv" sh -c 'printf "[defaults]\npeer_port=0\n" > /tmp/bad-port.conf'
out=$(docker exec "$P-srv" timeout 5 camex --mode server --port 5897 \
    --local-cidr 10.203.0.1/24 $SERVER_KEY_ARGS --psk "$PSK" \
    --config /tmp/bad-port.conf 2>&1)
rc=$?
[ $rc -ne 0 ] && [ $rc -ne 124 ] && echo "$out" | grep -q "Invalid peer_port: 0 (1 to 65535, or none)" &&
    echo "$out" | grep -q "/tmp/bad-port.conf, line 2: peer_port="
check $? "a server refuses peer_port=0 and names the line"
docker exec "$P-srv" sed -i '/^peer_port=none$/d' /etc/camex/camex.conf
docker kill -s HUP "$P-srv" >/dev/null
sleep 1
sync >/dev/null

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
out=$(docker exec -e NGINX_BIN=nginx-down -e INDEX="$INDEX" \
    "$P-srv" camex-nginx-sync 2>&1)
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

echo "== 7a. the domain from domain= in [server] (#106)"
# Every sync above ran without DOMAIN: the names came from the server
docker exec "$P-srv" grep -qx "# domain: $DOMAIN" /etc/nginx/camex-map.conf &&
    docker exec "$P-srv" grep -q "\"cam-a\.$DOMAIN\"" /etc/nginx/camex-map.conf
check $? "without DOMAIN the map takes the server's domain= ($DOMAIN)"
err=$(docker exec "$P-srv" camex --status --format nginx-map --domain other.test 2>&1 >/dev/null)
echo "$err" | grep -q -- "--domain other.test differs from the server's domain= $DOMAIN: other.test used"
check $? "--domain that differs from domain= is used, with a warning"
docker exec "$P-srv" sh -c 'echo "[server]" > /tmp/bad.conf; echo "domain=bad_domain" >> /tmp/bad.conf'
out=$(docker exec "$P-srv" timeout 5 camex --mode server --port 5898 \
    --local-cidr 10.202.0.1/24 $SERVER_KEY_ARGS --psk "$PSK" \
    --config /tmp/bad.conf 2>&1)
rc=$?
[ $rc -ne 0 ] && [ $rc -ne 124 ] && echo "$out" | grep -q "Invalid domain=: bad_domain"
check $? "a server refuses a file with a domain= that is no host name"

echo "== 8. IDs that are map keywords, no domain"
# Without a domain the keys are bare IDs: default, include, hostnames and
# volatile must stay hosts, not map's own parameters. The server's
# domain= is taken away, applied with a reload (kill -HUP)
docker exec "$P-srv" sed -i '/^domain=/d' /etc/camex/camex.conf
docker kill -s HUP "$P-srv" >/dev/null
sleep 1
for id in default include; do client "$id"; done
wait_registered default include
sleep 2
out=$(docker exec -e NGINX_BIN=nginx-counting "$P-srv" camex-nginx-sync 2>&1)
sleep 1
echo "$out" | sed 's/^/      /'
echo "$out" | grep -q "camex: no domain (domain= in \[server\] or --domain)" &&
    echo "$out" | grep -q "warning: no domain" &&
    docker exec "$P-srv" grep -qx "# domain: (none)" /etc/nginx/camex-map.conf
check $? "without domain= after a reload: the bare IDs, with a warning"
docker exec "$P-srv" grep -q '^    "\\default" ' /etc/nginx/camex-map.conf &&
    docker exec "$P-srv" nginx -t >/dev/null 2>&1
check $? "the map has them and nginx -t accepts it"
[ "$(get default)" = "I am default" ] && [ "$(get include)" = "I am include" ] &&
    get nobody | grep -q "is offline"
check $? "each one gets its own page, an unknown name still the offline page"

echo "== 9. a warning when nginx does not follow the domain (#106)"
docker exec "$P-srv" sh -c "sed -i '/^\\[server\\]/a domain=wrong.test' /etc/camex/camex.conf"
docker kill -s HUP "$P-srv" >/dev/null
sleep 1
out=$(sync)
echo "$out" | grep -q "warning: nginx has no server_name \*\.wrong\.test"
check $? "a domain nginx has no server_name for is reported"
out=$(docker exec -e NGINX_BIN=nginx-counting -e INDEX="$INDEX" "$P-srv" camex-nginx-sync --check 2>&1)
rc=$?
echo "$out" | sed 's/^/      /'
[ $rc -ne 0 ] && echo "$out" | grep -q "FAIL  nginx has no server_name \*\.wrong\.test" &&
    echo "$out" | grep -q "ok    nginx includes the map"
check $? "--check fails on it and says why"

echo "== 10. --follow: a sync at each change, none while nothing changes (#130)"
out=$(docker exec "$P-srv" camex --status --wait - 2>&1)
echo "      token: $out"
echo "$out" | grep -Eq '^[0-9a-f]{16}:/run/camex/camex-udp-5800.sock$'
check $? "camex --status --wait - prints the state's token at once (the stale socket left out)"
TOKEN=$out
t0=$(date +%s)
out=$(docker exec "$P-srv" camex --status --wait "$TOKEN" --wait-timeout 2 2>&1)
t1=$(date +%s)
[ "$out" = "$TOKEN" ] && [ $((t1 - t0)) -ge 2 ] && [ $((t1 - t0)) -le 4 ]
check $? "with no change it waits for --wait-timeout and gives the same token ($((t1 - t0)) s)"
# --control with a token of more servers: only that server counts
out=$(docker exec "$P-srv" sh -c "t0=\$(date +%s); camex --status --control /run/camex/camex-udp-5800.sock \
    --wait '$TOKEN,0000000000000000:/run/camex/other.sock' --wait-timeout 2 >/dev/null; echo \$((\$(date +%s) - t0))")
[ "$out" -ge 2 ] 2>/dev/null
check $? "--control waits for its own server, whatever else the token names (${out} s)"
# Only a socket left behind (section 5): "-" answers at once, "none"
# waits instead of spinning
out=$(docker exec "$P-srv" camex --status --control /run/camex/camex-udp-5899.sock --wait - 2>&1)
t0=$(date +%s)
out2=$(docker exec "$P-srv" camex --status --control /run/camex/camex-udp-5899.sock --wait none --wait-timeout 2 2>&1)
t1=$(date +%s)
[ "$out" = none ] && [ "$out2" = none ] && [ $((t1 - t0)) -ge 2 ]
check $? "a socket left behind: no state (none), and a wait on it takes its time ($((t1 - t0)) s)"
# A server that starts during the wait wakes it (#138), not its timeout
TOKEN=$(docker exec "$P-srv" camex --status --wait - 2>/dev/null)
docker exec -d "$P-srv" sh -c 'sleep 2; exec camex-run --mode server --port 5802 \
    --local-cidr 10.202.0.1/24 '"$SERVER_KEY_ARGS"' --psk "'"$PSK"'" > /tmp/srv5802.log 2>&1'
t0=$(date +%s)
out=$(docker exec "$P-srv" camex --status --wait "$TOKEN" --wait-timeout 30 2>&1)
t1=$(date +%s)
[ $((t1 - t0)) -le 6 ] && [ "$out" = "$TOKEN" ]
check $? "a server that starts during the wait ends it in $((t1 - t0)) s, not 30"
out=$(docker exec "$P-srv" camex --status --wait "$TOKEN" --wait-timeout 30 2>&1)
echo "$out" | grep -q "/run/camex/camex-udp-5802.sock"
check $? "the next wait has its state in the token"
docker exec "$P-srv" sh -c 'pkill -f "camex --mode server --port 5802"; true'
sleep 1
# The same where a server left its socket behind (section 5): a server
# started at that path is a new one too
TOKEN=$(docker exec "$P-srv" camex --status --wait - 2>/dev/null)
docker exec -d "$P-srv" sh -c 'sleep 2; exec camex-run --mode server --port 5899 \
    --local-cidr 10.204.0.1/24 '"$SERVER_KEY_ARGS"' --psk "'"$PSK"'" > /tmp/srv5899b.log 2>&1'
t0=$(date +%s)
docker exec "$P-srv" camex --status --wait "$TOKEN" --wait-timeout 30 >/dev/null 2>&1
t1=$(date +%s)
[ $((t1 - t0)) -le 6 ]
check $? "a server started where one left its socket ends the wait in $((t1 - t0)) s"
docker exec "$P-srv" sh -c 'pkill -f "camex --mode server --port 5899"; true'
sleep 1
# Counts the map runs of the sync script
docker exec "$P-srv" sh -c 'printf "#!/bin/sh\ncase \"\$*\" in *nginx-map*) echo x >> /tmp/maps;; esac\nexec camex \"\$@\"\n" \
    > /usr/local/bin/camex-counting && chmod +x /usr/local/bin/camex-counting && rm -f /tmp/maps'
docker exec -d -e CAMEX=camex-counting -e NGINX_BIN=nginx-counting "$P-srv" \
    sh -c 'exec camex-nginx-sync --follow 2>/tmp/follow.log'
sleep 3
maps() { docker exec "$P-srv" sh -c 'wc -l < /tmp/maps 2>/dev/null || echo 0'; }
m0=$(maps)
sleep 12
m1=$(maps)
[ "$m0" -ge 1 ] && [ "$m1" = "$m0" ]
check $? "after its first sync, no sync for 12 s while nothing changes ($m0 -> $m1)"
client CAM-F
for _ in $(seq 1 20); do
    docker logs "$P-srv" 2>&1 | grep -q "Registered auto client CAM-F" && break
    sleep 0.5
done
t0=$(date +%s)
for _ in $(seq 1 30); do
    docker exec "$P-srv" grep -q '"cam-f\.' /etc/nginx/camex-map.conf && break
    sleep 0.2
done
t1=$(date +%s)
docker exec "$P-srv" grep -q '"cam-f\.' /etc/nginx/camex-map.conf && [ $((t1 - t0)) -le 2 ]
check $? "a new client is in the map within 2 s of registering ($((t1 - t0)) s)"
docker rm -f "$P-CAM-F" >/dev/null
for _ in $(seq 1 40); do
    docker exec "$P-srv" grep -q '"cam-f\.' /etc/nginx/camex-map.conf || break
    sleep 0.5
done
! docker exec "$P-srv" grep -q '"cam-f\.' /etc/nginx/camex-map.conf
check $? "a client that is gone leaves the map as soon as the server drops it"
# A server that starts while --follow waits: its client in the map at once
# (#138); its own pool, so that the two tunnels do not overlap
docker exec "$P-srv" sh -c 'printf "[server]\nauto_pool=10.203.0.0/24\n" > /tmp/c5803.conf'
docker exec -d "$P-srv" sh -c 'exec camex-run --mode server --port 5803 \
    --local-cidr 10.203.0.1/24 --config /tmp/c5803.conf '"$SERVER_KEY_ARGS"' \
    --psk "'"$PSK"'" > /tmp/srv5803.log 2>&1'
client CAM-H 5803
for _ in $(seq 1 30); do
    docker exec "$P-srv" grep -q "Registered auto client CAM-H" /tmp/srv5803.log 2>/dev/null && break
    sleep 0.5
done
t0=$(date +%s)
for _ in $(seq 1 30); do
    docker exec "$P-srv" grep -q '"cam-h\.' /etc/nginx/camex-map.conf && break
    sleep 0.2
done
t1=$(date +%s)
docker exec "$P-srv" grep -q '"cam-h\.' /etc/nginx/camex-map.conf && [ $((t1 - t0)) -le 3 ]
check $? "a server started during --follow: its client in the map within 3 s ($((t1 - t0)) s)"
docker rm -f "$P-CAM-H" >/dev/null
docker exec "$P-srv" sh -c 'pkill -f "camex --mode server --port 5803"; true'
docker exec "$P-srv" sh -c 'pkill -f "camex-nginx-sync --follow"; true'
# A server before 3.11.0 answers WAIT with ERROR: camex exits 3
docker exec "$P-srv" sh -c 'printf "#!/bin/sh\ncase \"\$*\" in *--wait*) echo \"camex: too old\" >&2; exit 3;; esac\nexec camex \"\$@\"\n" \
    > /usr/local/bin/camex-old && chmod +x /usr/local/bin/camex-old'
docker exec -d -e CAMEX=camex-old -e NGINX_BIN=nginx-counting "$P-srv" \
    sh -c 'exec camex-nginx-sync --follow 2>/tmp/follow-old.log'
client CAM-G
for _ in $(seq 1 30); do
    docker exec "$P-srv" grep -q '"cam-g\.' /etc/nginx/camex-map.conf && break
    sleep 1
done
docker exec "$P-srv" grep -q '"cam-g\.' /etc/nginx/camex-map.conf &&
    docker exec "$P-srv" grep -q "cannot report its changes: a sync every 10 seconds" /tmp/follow-old.log
check $? "with a server too old for it: a sync every 10 seconds, said once"
docker exec "$P-srv" sh -c 'pkill -f "camex-nginx-sync --follow"; true'

echo "== 11. the start page refreshes itself in place (#142)"
# The page as the sync wrote it, a next one (other counts, a marked list)
# and one of another camex version; its script runs in jsdom with fetch
# answering those
JS_DIR="$BUILD_CTX/start-page"
mkdir -p "$JS_DIR"
cp "$REPO_DIR/tests/start-page/refresh-test.js" "$JS_DIR/"
docker exec "$P-srv" cat "$INDEX" > "$JS_DIR/a.html"
grep -q '<noscript><meta http-equiv="refresh" content="30"></noscript>' "$JS_DIR/a.html" &&
    grep -q '<meta name="camex-version" content="' "$JS_DIR/a.html"
check $? "the page has its version, and the 30-second reload only without JS"
sed -e 's|<div class="tabs">|<div class="tabs" data-t="new">|' \
    -e 's|<div class="list list-conf">|<div class="list list-conf" data-l="new">|' \
    -e 's|<div class="list list-auto">|<div class="list list-auto" data-l="new">|' \
    -e 's|<div class="sub">[0-9]* online|<div class="sub">99 online|' "$JS_DIR/a.html" > "$JS_DIR/b.html"
sed 's|<meta name="camex-version" content="[^"]*"|<meta name="camex-version" content="0.0.0"|' \
    "$JS_DIR/b.html" > "$JS_DIR/c.html"
# (the files go in through stdin: a temporary directory need not be one
# the Docker daemon sees)
# (a failure, npm unreachable too, is the check's to report, not set -e's)
rc=0
out=$(tar -C "$JS_DIR" -cf - . | docker run --rm -i -e HOME=/tmp -e npm_config_cache=/tmp/npm \
    node:24-slim sh -c 'mkdir -p /t && cd /t && tar -xf - &&
        { npm install --silent --no-audit --no-fund jsdom@24 >/dev/null 2>&1 ||
          { echo "FAIL: npm install jsdom@24"; exit 1; }; } &&
        node refresh-test.js' 2>&1) || rc=$?
echo "$out" | sed 's/^/      /'
[ $rc -eq 0 ] && ! echo "$out" | grep -q "^FAIL"
check $? "the page's script: swaps the lists in place, keeps the tab, marks no connection, reloads for a new version"

if [ "$FAILS" -ne 0 ]; then
    echo "FAIL: $FAILS check(s) failed"
    exit 1
fi
echo "PASS: nginx follows the camex clients by name"
