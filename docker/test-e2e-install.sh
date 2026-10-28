#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-e2e-install.sh — install.sh on Debian with systemd (#129)
#
# A Debian container runs systemd; install.sh gets camex and the files of
# examples/ through file:// URLs (CAMEX_RELEASE, CAMEX_SRC) and installs
# its packages with apt-get. A client container connects with what the
# installer printed.
#
# 1. Not root, not Debian: refused, nothing installed.
# 2. camex alone: key, settings, service; a client connects with the
#    printed public key and psk; camex --status answers.
# 3. Run again: nothing changes, camex is not restarted.
# 4. --domain: nginx in front of the client, domain= added to camex.conf,
#    the start page behind a generated password.
# 5. Run again without --domain: nginx's part stays and is kept up to date;
#    an edit of nginx's file stays; another domain is refused.
# 5a. The sync service follows a restarted server (#130).
# 6. The layout of camex-nginx-setup.sh (3.9 and before) moves to the new
#    names; a unit with its own CAMEX_ARGS keeps them in /etc/default/camex;
#    camex 3.10's timer is removed.
# 7. --uninstall lists the files and removes nothing.
# 8. Every option on a new host (#141): TCP, port, pool, psk, password,
#    nginx's port and the cameras' port; a backslash in the psk and the
#    password; a rerun with others (and -Rtcp in the unit) changes none.
#
# Usage: ./test-e2e-install.sh
#   CAMEX_BINARY=path/to/camex  binary to install (default: ../camex; it must
#                               run on Debian 12)
#   BASE_IMAGE=ubuntu:24.04     the system (default: debian:bookworm-slim)
#   KEEP=1                      keep containers for debugging
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/camex}"
BASE_IMAGE="${BASE_IMAGE:-debian:bookworm-slim}"
IMAGE="camex:e2e-install"
P="camex-install-$$"
NET_NAME="$P-net"
BUILD_CTX="$(mktemp -d)"
DOMAIN="cams.test"
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

# The release and the repository's files, as install.sh downloads them
mkdir -p "$BUILD_CTX/release" "$BUILD_CTX/src/examples"
cp "$CAMEX_BINARY" "$BUILD_CTX/release/camex-x86_64"
cp "$REPO_DIR/install.sh" "$BUILD_CTX/src/"
cp "$REPO_DIR"/examples/* "$BUILD_CTX/src/examples/"
cat > "$BUILD_CTX/Dockerfile" <<'DOCKERFILE'
ARG BASE_IMAGE
FROM $BASE_IMAGE
# systemd as in a server; nothing that install.sh installs itself (curl,
# nginx, openssl)
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
        systemd systemd-sysv dbus iproute2 busybox >/dev/null && \
    rm -rf /var/lib/apt/lists/* && \
    rm -f /lib/systemd/system/multi-user.target.wants/* \
          /etc/systemd/system/*.wants/* \
          /lib/systemd/system/local-fs.target.wants/* \
          /lib/systemd/system/sockets.target.wants/*udev* \
          /lib/systemd/system/sysinit.target.wants/systemd-tmpfiles-setup* && \
    systemctl mask systemd-udevd.service systemd-udevd-kernel.socket \
        systemd-udevd-control.socket getty.target console-getty.service
COPY release /opt/release
COPY src /opt/src
STOPSIGNAL SIGRTMIN+3
CMD ["/lib/systemd/systemd"]
DOCKERFILE
docker build -q --build-arg BASE_IMAGE="$BASE_IMAGE" -t "$IMAGE" "$BUILD_CTX" >/dev/null
docker network create "$NET_NAME" >/dev/null
set +e   # checks below report failures themselves

# The server: systemd as PID 1 (privileged: cgroups and /dev/net/tun)
docker run -d --name "$P-srv" --hostname srv --network "$NET_NAME" --privileged \
    --cgroupns=private --tmpfs /run --tmpfs /run/lock "$IMAGE" >/dev/null
for _ in $(seq 1 30); do
    state=$(docker exec "$P-srv" systemctl is-system-running 2>/dev/null)
    case "$state" in running|degraded) break ;; esac
    sleep 1
done
echo "   systemd: $state"
# New clients right after the server starts: no start hold (#81)
docker exec "$P-srv" sh -c 'mkdir -p /etc/systemd/system/camex.service.d &&
    printf "[Service]\nEnvironment=CAMEX_POOL_HOLD=0\n" > /etc/systemd/system/camex.service.d/test.conf'

run() {     # run <args>: install.sh as root, as the pipe from GitHub would
    docker exec -e CAMEX_SRC=file:///opt/src -e CAMEX_RELEASE=file:///opt/release \
        "$P-srv" sh -c 'cat /opt/src/install.sh | sh -s -- "$@"' sh "$@" 2>&1
}
x() {       # x <command>: in the server
    docker exec "$P-srv" sh -c "$1"
}
pid() {
    x 'systemctl show -p MainPID --value camex'
}

echo "== 1. refused: not root, not Debian"
out=$(docker exec -u nobody -e CAMEX_SRC=file:///opt/src "$P-srv" sh /opt/src/install.sh 2>&1)
[ $? -ne 0 ] && echo "$out" | grep -q "run it as root"
check $? "not root: refused"
x 'mv /etc/os-release /etc/os-release.real && printf "ID=fedora\nPRETTY_NAME=\"Fedora 40\"\n" > /etc/os-release'
out=$(run)
rc=$?
x 'rm /etc/os-release && mv /etc/os-release.real /etc/os-release'
[ $rc -ne 0 ] && echo "$out" | grep -q "this is Fedora 40, not Debian or Ubuntu" &&
    ! x 'test -e /usr/local/bin/camex || test -e /etc/camex'
check $? "Fedora: refused with a message, nothing installed"
out=$(run --domain 'bad_domain')
[ $? -ne 0 ] && echo "$out" | grep -q "not a domain"
check $? "a bad domain is refused"
long=$(printf 'a%.0s' $(seq 1 60)); long="$long.$long.$long.$long.$long.test"
out=$(run --domain "$long")
[ $? -ne 0 ] && echo "$out" | grep -q "not a domain"
check $? "a domain over 253 characters is refused (camex would not start with it)"

echo "== 2. camex alone"
out=$(run)
rc=$?
echo "$out" | sed 's/^/      /'
[ $rc -eq 0 ] && echo "$out" | grep -q '^\[8/8\] Done' &&
    echo "$out" | grep -q "^  1\. Check the host"
check $? "the installer lists its steps and ends"
x 'systemctl is-active --quiet camex && systemctl is-enabled --quiet camex'
check $? "camex.service is enabled and running"
[ "$(x 'stat -c %a /etc/camex/server.key /etc/camex/camex.conf /etc/camex' | tr '\n' ' ')" = "600 600 700 " ]
check $? "server.key and camex.conf for root only"
x 'grep -qE "^psk=hex:[0-9a-f]{64}$" /etc/camex/camex.conf && grep -qx "port=5800" /etc/camex/camex.conf &&
   ! grep -q "^domain=" /etc/camex/camex.conf && grep -qx "client_timeout=60" /etc/camex/camex.conf'
check $? "camex.conf has a new psk, port 5800, client_timeout=60, no domain"
x 'grep -qx "CAMEX_ARGS=\"--mode server --config /etc/camex/camex.conf --server-key-file /etc/camex/server.key\"" /etc/default/camex'
check $? "/etc/default/camex holds the server's arguments"
x 'dpkg -s curl ca-certificates >/dev/null 2>&1 && ! dpkg -s nginx >/dev/null 2>&1'
check $? "curl installed, nginx not"
PUBKEY=$(x "sed -n 's/^public_key=//p' /etc/camex/server.key")
PSK=$(x "sed -n 's/^psk=//p' /etc/camex/camex.conf")
echo "$out" | grep -qF -- "--server-pubkey $PUBKEY --psk '$PSK'"
check $? "the client's command line has the public key and psk"
# A client with what was printed; it serves its ID on port 80
docker run -d --name "$P-cli" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD \
    "$IMAGE" sh -c \
    'mkdir -p /dev/net /www && { [ -e /dev/net/tun ] || mknod /dev/net/tun c 10 200; } &&
     echo "I am CAM1" > /www/index.html && busybox httpd -p 80 -h /www &&
     exec /opt/release/camex-x86_64 --mode client --auto --name CAM1 --server-host '"$P-srv"' --port 5800 \
         --server-pubkey '"$PUBKEY"' --psk '"'$PSK'"' --keepalive 2' >/dev/null
for _ in $(seq 1 20); do
    x 'camex --status' 2>/dev/null | grep -q CAM1 && break
    sleep 1
done
x 'camex --status' | sed 's/^/      /'
x 'camex --status' | grep -q CAM1
check $? "the client connects; camex --status answers (control socket in /run/camex)"

echo "== 3. run again"
p1=$(pid)
out=$(run)
rc=$?
[ $rc -eq 0 ] && echo "$out" | grep -q "unchanged: /usr/local/bin/camex" &&
    echo "$out" | grep -q "kept: /etc/camex/camex.conf" &&
    [ "$(x "sed -n 's/^psk=//p' /etc/camex/camex.conf")" = "$PSK" ] && [ "$(pid)" = "$p1" ]
check $? "nothing changes, the psk stays, camex keeps running (pid $p1)"

echo "== 4. --domain $DOMAIN"
out=$(run --domain "$DOMAIN")
rc=$?
echo "$out" | sed 's/^/      /'
[ $rc -eq 0 ] && x "grep -qx 'domain=$DOMAIN' /etc/camex/camex.conf" &&
    [ "$(x "sed -n 's/^psk=//p' /etc/camex/camex.conf")" = "$PSK" ]
check $? "domain= added to camex.conf, the rest kept"
x 'systemctl is-active --quiet nginx && systemctl is-active --quiet camex-nginx-sync.service &&
   systemctl is-enabled --quiet camex-nginx-sync.service && test ! -e /etc/systemd/system/camex-nginx-sync.timer &&
   test -x /usr/local/bin/camex-nginx-sync && test -f /etc/nginx/conf.d/camex-nginx.conf &&
   test ! -e /etc/nginx/conf.d/camex.conf'
check $? "nginx and the sync script's service run (no timer), under the new names"
x "grep -q 'server_name \*\.$DOMAIN;' /etc/nginx/conf.d/camex-nginx.conf"
check $? "nginx's file has the domain"
PW=$(x 'cat /etc/camex/nginx-password')
[ -n "$PW" ] && [ "$(x 'stat -c "%a" /etc/camex/nginx-password')" = 600 ] &&
    [ "$(x 'stat -c "%a %G" /etc/nginx/camex.htpasswd')" = "640 www-data" ] &&
    echo "$out" | grep -q "user camex, password $PW"
check $? "the start page's password: made, kept for root, printed"
echo "$out" | grep -q "ok    nginx sends camex-check-[0-9]*\.$DOMAIN to camex"
check $? "the installer's --check sees nginx answer for the domain"
page() {    # page <host> [<user:password>]: status and body from nginx
    x "curl -s -m 10 -w '\n%{http_code}' ${2:+-u '$2'} -H 'Host: $1' http://127.0.0.1/"
}
page "cam1.$DOMAIN" | grep -q "I am CAM1"
check $? "the client's web UI at cam1.$DOMAIN"
[ "$(page "$DOMAIN" | tail -n 1)" = 401 ] && page "$DOMAIN" "camex:$PW" | grep -qF "//cam1.$DOMAIN/"
check $? "the start page lists it, behind the password"

echo "== 5. run again without --domain, an edit, another domain"
x 'echo "# my edit" >> /etc/nginx/conf.d/camex-nginx.conf'
out=$(run)
rc=$?
[ $rc -eq 0 ] && echo "$out" | grep -q "kept: /etc/nginx/conf.d/camex-nginx.conf" &&
    echo "$out" | grep -q "unchanged: /usr/local/bin/camex-nginx-sync" &&
    x 'grep -qx "# my edit" /etc/nginx/conf.d/camex-nginx.conf' &&
    [ "$(x 'cat /etc/camex/nginx-password')" = "$PW" ]
check $? "nginx's part is kept up to date, the edit and the password stay"
# Another user in nginx's file, a new password, a per-client psk=
x 'echo "admin:\$apr1\$x\$y" >> /etc/nginx/camex.htpasswd && rm /etc/camex/nginx-password &&
   printf "\n[client CAM9]\npsk=only-for-cam9\n" >> /etc/camex/camex.conf'
out=$(run)
PW2=$(x 'cat /etc/camex/nginx-password')
[ $? -eq 0 ] && [ -n "$PW2" ] && [ "$PW2" != "$PW" ] && x 'grep -q "^admin:" /etc/nginx/camex.htpasswd' &&
    [ "$(x 'grep -c "^camex:" /etc/nginx/camex.htpasswd')" = 1 ] &&
    [ "$(page "$DOMAIN" "camex:$PW2" | tail -n 1)" = 200 ] && [ "$(page "$DOMAIN" "camex:$PW" | tail -n 1)" = 401 ]
check $? "a new password replaces only the camex line; other users stay"
echo "$out" | grep -qF -- "--psk '$PSK'"
check $? "the printed psk is the server's, not a [client ID] one"
PW=$PW2
# Another nginx file with the map: refused in step 1, before camex.conf
x 'echo "include /etc/nginx/camex-map.conf;" > /etc/nginx/conf.d/zz-other.conf'
out=$(run --domain "$DOMAIN")
rc=$?
x 'rm /etc/nginx/conf.d/zz-other.conf'
[ $rc -ne 0 ] && echo "$out" | grep -q "zz-other.conf is a camex file of nginx too" &&
    ! echo "$out" | grep -q '^\[2/8\]'
check $? "another nginx file with the map stops it in step 1, before anything changes"
out=$(run --domain other.test)
[ $? -ne 0 ] && echo "$out" | grep -q "has domain=$DOMAIN, not other.test" &&
    x "grep -qx 'domain=$DOMAIN' /etc/camex/camex.conf"
check $? "another domain is refused, nothing changed"

echo "== 5a. the service follows a restarted server (#130)"
x 'systemctl restart camex'
for _ in $(seq 1 90); do
    page "cam1.$DOMAIN" | grep -q "I am CAM1" && break
    sleep 1
done
page "cam1.$DOMAIN" | grep -q "I am CAM1" && x 'systemctl is-active --quiet camex-nginx-sync.service'
check $? "after systemctl restart camex the client is back in the map"

echo "== 6. the layouts of camex-nginx-setup.sh and of camex 3.10"
# camex 3.10: the sync once from a timer; before: other names
x 'systemctl disable --now camex-nginx-sync.service
   printf "[Timer]\nOnUnitActiveSec=10s\n[Install]\nWantedBy=timers.target\n" > /etc/systemd/system/camex-nginx-sync.timer
   systemctl daemon-reload && systemctl enable --now camex-nginx-sync.timer
   mv /etc/nginx/conf.d/camex-nginx.conf /etc/nginx/conf.d/camex.conf
   mv /usr/local/bin/camex-nginx-sync /usr/local/bin/camex-nginx-sync.sh
   sed -i "s|^ExecStart=.*|ExecStart=/usr/local/bin/camex-nginx-sync.sh|; s|^Environment=INDEX=.*|Environment=INDEX=/var/lib/camex-nginx/start.html|" \
       /etc/systemd/system/camex-nginx-sync.service
   rm /etc/default/camex
   printf "[Service]\nExecStart=/usr/local/bin/camex \$CAMEX_ARGS\nEnvironment=CAMEX_ARGS=--mode server --config /etc/camex/camex.conf --server-key-file /etc/camex/server.key --mtu 1400\n" \
       > /etc/systemd/system/camex.service
   systemctl daemon-reload && systemctl restart camex'
out=$(run)
rc=$?
echo "$out" | sed 's/^/      /'
[ $rc -eq 0 ] && x 'test -f /etc/nginx/conf.d/camex-nginx.conf && test ! -e /etc/nginx/conf.d/camex.conf &&
    grep -qx "# my edit" /etc/nginx/conf.d/camex-nginx.conf &&
    test ! -e /usr/local/bin/camex-nginx-sync.sh && ls /var/backups/camex | grep -q camex-nginx-sync.sh'
check $? "nginx's file renamed with its edits, the old sync script removed (a copy kept)"
x 'grep -qx "Environment=INDEX=/var/lib/camex-nginx/start.html" /etc/systemd/system/camex-nginx-sync.service'
check $? "the start page's path of the old service is kept"
x 'test ! -e /etc/systemd/system/camex-nginx-sync.timer && ! systemctl is-active --quiet camex-nginx-sync.timer &&
   ls /var/backups/camex | grep -q camex-nginx-sync.timer'
check $? "camex 3.10's timer stopped and removed (a copy kept)"
x 'grep -q -- "--mtu 1400" /etc/default/camex && [ "$(stat -c %a /etc/default/camex)" = 600 ] && grep -q "^EnvironmentFile=/etc/default/camex" /etc/systemd/system/camex.service &&
   systemctl is-active --quiet camex && systemctl is-active --quiet camex-nginx-sync.service'
check $? "the old unit's arguments moved to /etc/default/camex, the services run"
# The client finds the restarted server again (--server-timeout), the
# sync service puts it back in the map
for _ in $(seq 1 90); do
    page "cam1.$DOMAIN" | grep -q "I am CAM1" && break
    sleep 1
done
page "cam1.$DOMAIN" | grep -q "I am CAM1"
check $? "the client's web UI still opens"
x 'systemctl reload camex' && sleep 1 && x 'systemctl is-active --quiet camex'
check $? "systemctl reload camex (kill -HUP)"

echo "== 7. --uninstall"
before=$(x 'ls -R /etc/camex /etc/nginx /usr/local/bin /etc/systemd/system /etc/default /var/lib/camex-nginx 2>&1 | md5sum')
out=$(run --uninstall)
rc=$?
echo "$out" | sed 's/^/      /'
after=$(x 'ls -R /etc/camex /etc/nginx /usr/local/bin /etc/systemd/system /etc/default /var/lib/camex-nginx 2>&1 | md5sum')
[ $rc -eq 0 ] && [ "$before" = "$after" ] && x 'systemctl is-active --quiet camex'
check $? "it removes and stops nothing"
echo "$out" | grep -qx "  + /usr/local/bin/camex" && echo "$out" | grep -qx "  + /etc/nginx/conf.d/camex-nginx.conf" &&
    echo "$out" | grep -qx "  - /usr/local/bin/camex-nginx-sync.sh" &&
    echo "$out" | grep -qx "  rm /usr/local/bin/camex-nginx-sync" &&
    echo "$out" | grep -qx "  systemctl disable --now camex-nginx-sync.service camex.service"
check $? "it lists the files, which are there, and the commands to remove them"

echo "== 8. every option, on a new host; a rerun with others changes nothing (#141)"
docker run -d --name "$P-opt" --hostname opt --network "$NET_NAME" --privileged \
    --cgroupns=private --tmpfs /run --tmpfs /run/lock "$IMAGE" >/dev/null
for _ in $(seq 1 30); do
    case "$(docker exec "$P-opt" systemctl is-system-running 2>/dev/null)" in
        running|degraded) break ;;
    esac
    sleep 1
done
o() {       # o <command> [<arg>...]: in the second server, the args as $1...
    c=$1; shift
    docker exec "$P-opt" sh -c "$c" sh "$@"
}
# New clients right after the server starts: no start hold (#81)
o 'mkdir -p /etc/systemd/system/camex.service.d &&
   printf "[Service]\nEnvironment=CAMEX_POOL_HOLD=0\n" > /etc/systemd/system/camex.service.d/test.conf'
orun() {    # orun <args>: install.sh there
    docker exec -e CAMEX_SRC=file:///opt/src -e CAMEX_RELEASE=file:///opt/release \
        "$P-opt" sh -c 'cat /opt/src/install.sh | sh -s -- "$@"' sh "$@" 2>&1
}
# A backslash in the psk and the password: written and printed as given,
# not as an echo of dash reads it; a quote in the password: --check opens
# the start page with it
OPSK='my long\cpass\nphrase'
out=$(orun --domain opts.test --port 5900 --transport tcp --pool 10.60.1.7/22 \
    --psk "$OPSK" --password 'pa:ss\c "word' --http-port 85 --upstream-port 8080 \
    --client-timeout 90)
rc=$?
echo "$out" | sed 's/^/      /'
[ $rc -eq 0 ] && o 'grep -qx "local_cidr=10.60.0.1/22" /etc/camex/camex.conf &&
    grep -qx "auto_pool=10.60.0.0/22" /etc/camex/camex.conf &&
    grep -qxF "psk=$1" /etc/camex/camex.conf && grep -qx "port=5900" /etc/camex/camex.conf &&
    grep -qx "client_timeout=90" /etc/camex/camex.conf' "$OPSK"
check $? "camex.conf: port, the pool's network with the server first, the given psk, client_timeout"
o 'grep -q -- "--transport tcp" /etc/default/camex && camex --status | grep -q "tcp port 5900"'
check $? "the server runs over TCP on the given port"
echo "$out" | grep -qF -- "--port 5900 --transport tcp" && echo "$out" | grep -qF -- "--psk '$OPSK'" &&
    echo "$out" | grep -qF -- '(user camex, password pa:ss\c "word)'
check $? "the printed client command has the transport, the port and the psk"
o 'grep -qE "^[[:space:]]*listen[[:space:]]+85;" /etc/nginx/conf.d/camex-nginx.conf &&
   ! grep -qE "^[[:space:]]*listen[[:space:]]+80;" /etc/nginx/conf.d/camex-nginx.conf &&
   grep -qx "Environment=HTTP_PORT=85" /etc/systemd/system/camex-nginx-sync.service &&
   grep -qx "Environment=UPSTREAM_PORT=8080" /etc/systemd/system/camex-nginx-sync.service'
check $? "nginx listens on 85; the sync service has HTTP_PORT and UPSTREAM_PORT"
code() {    # code [<user:password>]: the start page's status on port 85
    o "curl -s -o /dev/null -w '%{http_code}' -m 10 ${1:+-u '$1'} -H 'Host: opts.test' http://127.0.0.1:85/"
}
[ "$(code)" = 401 ] && [ "$(code 'camex:pa:ss\c "word')" = 200 ]
check $? "the start page on port 85 opens with the given password"
echo "$out" | grep -q "ok    nginx sends camex-check-[0-9]*\.opts\.test to camex" &&
    echo "$out" | grep -q "ok    nginx serves the start page on opts.test" &&
    echo "$out" | grep -q "Start page:       http://opts.test:85/"
check $? "--check asks nginx on port 85; the addresses printed with :85"
# A client over TCP: the map leads to its web port 8080, the page's link to
# nginx's port 85
OPUB=$(o "sed -n 's/^public_key=//p' /etc/camex/server.key")
docker run -d --name "$P-ocli" --network "$NET_NAME" --cap-add NET_ADMIN --cap-add MKNOD -e OPSK="$OPSK" \
    "$IMAGE" sh -c 'mkdir -p /dev/net && { [ -e /dev/net/tun ] || mknod /dev/net/tun c 10 200; } &&
     exec /opt/release/camex-x86_64 --mode client --auto --name CAM2 --server-host '"$P-opt"' \
         --port 5900 --transport tcp --server-pubkey '"$OPUB"' --psk "$OPSK" \
         --keepalive 2' >/dev/null
for _ in $(seq 1 30); do
    o 'grep -q "\"cam2\.opts\.test\" 10\.60\.0\.[0-9]*:8080;" /etc/nginx/camex-map.conf' && break
    sleep 1
done
o 'grep -q "\"cam2\.opts\.test\" 10\.60\.0\.[0-9]*:8080;" /etc/nginx/camex-map.conf &&
   grep -q "href=\"http://cam2.opts.test:85/\"" /var/lib/camex-nginx/index.html'
check $? "a TCP client: the map leads to its port 8080, the start page links it on :85"
# The short form of an older unit's arguments: still TCP (camex reads -Rtcp)
o 'sed -i "s/--transport tcp/-Rtcp/" /etc/default/camex && grep -q -- "-Rtcp" /etc/default/camex'
out=$(orun --domain opts.test --port 5901 --transport udp --pool 10.70.0.0/24 --psk other \
    --password other --http-port 86 --upstream-port 9090 --client-timeout 120)
rc=$?
[ $rc -eq 0 ] && echo "$out" | grep -q "kept port= of /etc/camex/camex.conf, not --port 5901" &&
    echo "$out" | grep -q "kept psk= of /etc/camex/camex.conf, not --psk" &&
    echo "$out" | grep -q "kept auto_pool= of /etc/camex/camex.conf, not --pool 10.70.0.0/24" &&
    echo "$out" | grep -q "kept the server's transport tcp, not --transport udp" &&
    echo "$out" | grep -q "kept client_timeout= of /etc/camex/camex.conf, not --client-timeout 120" &&
    echo "$out" | grep -q "kept the start page's password, not --password" &&
    echo "$out" | grep -q "kept the listen lines of /etc/nginx/conf.d/camex-nginx.conf, not --http-port 86" &&
    echo "$out" | grep -q "kept UPSTREAM_PORT=8080 of camex-nginx-sync.service, not 9090" &&
    o 'grep -qx "Environment=UPSTREAM_PORT=8080" /etc/systemd/system/camex-nginx-sync.service &&
       grep -qx "Environment=HTTP_PORT=85" /etc/systemd/system/camex-nginx-sync.service' &&
    echo "$out" | grep -qF -- "--port 5900 --transport tcp" &&
    o 'grep -qxF "psk=$1" /etc/camex/camex.conf && grep -qx "port=5900" /etc/camex/camex.conf' "$OPSK" &&
    [ "$(code 'camex:pa:ss\c "word')" = 200 ]
check $? "a rerun with other values changes none of them and says what to edit"
# nginx's file made again: the sync service takes its port, not the old one;
# the service gone: made again with the port of the kept file
o 'rm /etc/nginx/conf.d/camex-nginx.conf'
out=$(orun --domain opts.test --http-port 87)
[ $? -eq 0 ] && o 'grep -qE "^[[:space:]]*listen[[:space:]]+87;" /etc/nginx/conf.d/camex-nginx.conf &&
    grep -qx "Environment=HTTP_PORT=87" /etc/systemd/system/camex-nginx-sync.service' &&
    echo "$out" | grep -q "Start page:       http://opts.test:87/"
check $? "a new nginx file on --http-port 87: the sync service and the addresses follow it"
o 'systemctl disable --now camex-nginx-sync.service >/dev/null 2>&1;
   rm /etc/systemd/system/camex-nginx-sync.service'
out=$(orun --domain opts.test)
[ $? -eq 0 ] && o 'grep -qx "Environment=HTTP_PORT=87" /etc/systemd/system/camex-nginx-sync.service' &&
    echo "$out" | grep -q "ok    nginx serves the start page on opts.test"
check $? "the sync service made again takes port 87 of the kept nginx file"
# listen with an address; CAMEX_ARGS with more blanks before tcp
o 'sed -i "s/^\([[:space:]]*listen[[:space:]]*\)87;/\10.0.0.0:87;/" /etc/nginx/conf.d/camex-nginx.conf &&
   grep -q "listen 0.0.0.0:87;" /etc/nginx/conf.d/camex-nginx.conf &&
   sed -i "s/-Rtcp/--transport   tcp/" /etc/default/camex'
out=$(orun --domain opts.test --transport tcp)
[ $? -eq 0 ] && o 'grep -qx "Environment=HTTP_PORT=87" /etc/systemd/system/camex-nginx-sync.service' &&
    ! echo "$out" | grep -q "kept the server's transport" &&
    echo "$out" | grep -qF -- "--transport tcp"
check $? "listen 0.0.0.0:87 is port 87; --transport   tcp in CAMEX_ARGS is TCP"
# camex.conf without client_timeout= (made before 3.13.0): camex uses 120 s,
# --client-timeout 120 is no change; 90 is
o 'sed -i "/^client_timeout=/d" /etc/camex/camex.conf'
out=$(orun --client-timeout 120)
[ $? -eq 0 ] && ! echo "$out" | grep -q "kept client_timeout=" &&
    out=$(orun --client-timeout 90) &&
    echo "$out" | grep -q "kept client_timeout= of /etc/camex/camex.conf, not --client-timeout 90"
check $? "no client_timeout= in camex.conf: --client-timeout 120 is the same, 90 is said"
out=$(orun --password x)
[ $? -ne 0 ] && echo "$out" | grep -q "give --domain"
check $? "--password without --domain is refused"
bad=0
for psk in "" " lead" "trail " "hex:$(printf '%064d' 0)" "base64:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=" \
           "base64:short=" "$(printf 'x%.0s' $(seq 1 128))" "$(printf 'é%.0s' $(seq 1 64))"; do
    out=$(orun --psk "$psk")
    [ $? -ne 0 ] && echo "$out" | grep -q -- "--psk" || { bad=1; echo "      accepted: '$psk'"; }
done
[ $bad = 0 ]
check $? "--psk: empty, blanks at its ends, all-zero keys, bad base64, 128 bytes (also in 64 letters) refused"

if [ "$FAILS" -ne 0 ]; then
    echo "FAIL: $FAILS check(s) failed"
    exit 1
fi
echo "PASS: install.sh installs camex and nginx on Debian"
