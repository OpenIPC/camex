#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# device-test.sh — end-to-end test of the Android app on a real device
#
# Runs a camex server in Docker on this host, makes it reachable from the
# device as 127.0.0.1:5800 through `adb reverse` (TCP transport), installs
# the debug APK, writes the settings with run-as, starts the VPN and checks:
#   - the VPN interface gets the address, MTU and route from CONFIG (#19)
#   - only the tunnel subnet and the CONFIG routes go through the VPN
#     (full tunnel off: a 0.0.0.0/0 from CONFIG is skipped; a route with
#     host bits set is installed as its network), and the app itself is
#     excluded from it (#18)
#   - the server reaches the device through the tunnel (ping)
#   - a changed profile moves the VPN to the new address, MTU and route
#     without restarting the app (#49)
#   - stopping the service stops camex
#
# Usage: tests/android/device-test.sh <adb-serial> <camex-android-debug.apk>
#   CAMEX_BINARY=path/to/camex  server binary (default: ./camex)
#   SERVER_HOST=<addr>          address of this host as seen from the device;
#                               the app connects there directly (port 5800
#                               is published on this host for the test)
#   DEVTEST_SUBNET=<cidr>       subnet of the server's Docker network
#                               (default 10.250.0.0/24)
#
# The server runs in its own Docker network, not in the default bridge:
# that is 172.17.0.0/16, and a device whose address falls inside it (for
# example 172.17.32.x behind another interface) gets no TCP answers, as
# the container sends them to its own bridge instead of back through the
# host.
#
# Without SERVER_HOST the app uses 127.0.0.1:5800 through `adb reverse`.
# That only works until the VPN is up: Android then closes the sockets of
# apps that move into the VPN, and adbd (which carries `adb reverse`) is
# one of them, so the tunnel connection drops and reconnects are refused.
# Use SERVER_HOST for the full test.
#
# The device must allow `adb shell` (uid 2000). The VPN consent is granted
# with `appops`; if that is not possible the system dialog is confirmed by
# tapping its OK button.
#

set -e

SERIAL="${1:?adb serial}"
APK="${2:?debug APK}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
CAMEX_BINARY="${CAMEX_BINARY:-$REPO_DIR/camex}"
PKG=org.openipc.camex
SRV="camex-devtest-srv-$$"
NET="camex-devtest-net-$$"
CTX="$(mktemp -d)"
FAILS=0
ADB="adb -s $SERIAL"

# Activity that was in front before the test (another app may be running
# on a shared device); brought back at the end.
PREV=$($ADB shell dumpsys activity activities 2>/dev/null |
    sed -nE 's/.*mResumedActivity: ActivityRecord\{[^ ]+ [^ ]+ ([^ ]+) .*/\1/p' | head -1 | tr -d '\r')

cleanup() {
    $ADB shell am force-stop "$PKG" >/dev/null 2>&1 || true
    if [ -n "$PREV" ] && [ "${PREV%%/*}" != "$PKG" ]; then
        $ADB shell am start -n "$PREV" >/dev/null 2>&1 || true
    fi
    $ADB reverse --remove tcp:5800 >/dev/null 2>&1 || true
    docker rm -f "$SRV" >/dev/null 2>&1 || true
    docker network rm "$NET" >/dev/null 2>&1 || true
    rm -rf "$CTX"
}
trap cleanup EXIT INT TERM

check() { if [ "$1" = 0 ]; then echo "   ok: $2"; else echo "   FAIL: $2"; FAILS=$((FAILS + 1)); fi; }

# ---------------------------------------------------------------- server
cp "$CAMEX_BINARY" "$CTX/camex"
cat > "$CTX/Dockerfile" <<'EOF'
FROM ubuntu:24.04
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
        iputils-ping >/dev/null && rm -rf /var/lib/apt/lists/*
COPY camex /usr/local/bin/camex
RUN printf '#!/bin/sh\nmkdir -p /dev/net\n[ -e /dev/net/tun ] || mknod /dev/net/tun c 10 200\nexec /usr/local/bin/camex "$@"\n' \
        > /usr/local/bin/camex-run && chmod +x /usr/local/bin/camex-run
ENTRYPOINT ["/usr/local/bin/camex-run"]
EOF
mkdir -p "$CTX/etc-camex"
cat > "$CTX/etc-camex/camex.conf" <<'EOF'
[server]
[client DEVTEST]
local_cidr=10.200.0.7/24
gateway_ip=10.200.0.1
mtu=1400
route_cidr=192.168.77.0/24
route_cidr=192.168.78.5/24
route_cidr=0.0.0.0/0
EOF
. "$REPO_DIR/docker/lib-keys.sh"
gen_server_key "$CTX/etc-camex" "$CAMEX_BINARY" || { echo "cannot create a server key"; exit 1; }
docker build -q -t camex:devtest "$CTX" >/dev/null
docker network create --subnet "${DEVTEST_SUBNET:-10.250.0.0/24}" "$NET" >/dev/null
docker create --name "$SRV" --network "$NET" -p 5800:5800 --cap-add NET_ADMIN --cap-add MKNOD \
    camex:devtest --mode server --port 5800 --transport tcp \
    --local-cidr 10.200.0.1/24 --psk devtest-key \
    --server-key-file /etc/camex/server.key \
    --config /etc/camex/camex.conf >/dev/null
docker cp "$CTX/etc-camex" "$SRV:/etc/camex" >/dev/null
docker start "$SRV" >/dev/null
if [ -z "$SERVER_HOST" ]; then
    $ADB reverse tcp:5800 tcp:5800 >/dev/null
fi

# ---------------------------------------------------------------- app
echo "== install $APK"
# CI signs every debug build with a fresh key: replace an older install
if ! $ADB install -r -g "$APK" >/dev/null 2>"$CTX/install.err"; then
    if grep -q INSTALL_FAILED_UPDATE_INCOMPATIBLE "$CTX/install.err"; then
        $ADB uninstall "$PKG" >/dev/null
        $ADB install -g "$APK" >/dev/null
    else
        cat "$CTX/install.err"; exit 1
    fi
fi
$ADB shell am force-stop "$PKG"

# Settings: server through adb reverse, TCP, server key, full tunnel OFF so
# the rest of the device keeps its normal network.
cat > "$CTX/camex.xml" <<'EOF'
<?xml version='1.0' encoding='utf-8' standalone='yes' ?>
<map>
    <string name="server_host">__SERVER_HOST__</string>
    <int name="server_port" value="5800" />
    <string name="name">DEVTEST</string>
    <string name="transport">tcp</string>
    <string name="server_pubkey">__SERVER_PUBKEY__</string>
    <string name="psk">devtest-key</string>
    <boolean name="full_tunnel" value="false" />
    <int name="mtu" value="1500" />
    <int name="keepalive" value="5" />
    <int name="server_timeout" value="15" />
</map>
EOF
sed -i -e "s/__SERVER_HOST__/${SERVER_HOST:-127.0.0.1}/" \
    -e "s|__SERVER_PUBKEY__|$SERVER_PUBKEY|" "$CTX/camex.xml"
$ADB push "$CTX/camex.xml" /data/local/tmp/camex.xml >/dev/null
$ADB shell "run-as $PKG sh -c 'mkdir -p shared_prefs && cat /data/local/tmp/camex.xml > shared_prefs/camex.xml'"
$ADB shell rm /data/local/tmp/camex.xml

# VPN consent
$ADB shell cmd appops set "$PKG" ACTIVATE_VPN allow >/dev/null 2>&1 || true

# ---------------------------------------------------------------- start
tap_text() {   # tap the UI element whose text matches $1 (regex)
    local xy
    $ADB shell uiautomator dump /data/local/tmp/ui.xml >/dev/null 2>&1
    xy=$($ADB shell cat /data/local/tmp/ui.xml | tr '>' '\n' |
        grep -iE "text=\"[^\"]*($1)[^\"]*\"" | head -1 |
        sed -nE 's/.*bounds="\[([0-9]+),([0-9]+)\]\[([0-9]+),([0-9]+)\]".*/\1 \2 \3 \4/p' |
        awk '{ print int(($1 + $3) / 2), int(($2 + $4) / 2) }')
    [ -n "$xy" ] || return 1
    $ADB shell input tap $xy
}

tap_exact() {  # tap the UI element whose text is exactly $1 ("Stop", not "stopped")
    local xy
    $ADB shell uiautomator dump /data/local/tmp/ui.xml >/dev/null 2>&1
    xy=$($ADB shell cat /data/local/tmp/ui.xml | tr '>' '\n' |
        grep -iF "text=\"$1\"" | head -1 |
        sed -nE 's/.*bounds="\[([0-9]+),([0-9]+)\]\[([0-9]+),([0-9]+)\]".*/\1 \2 \3 \4/p' |
        awk '{ print int(($1 + $3) / 2), int(($2 + $4) / 2) }')
    [ -n "$xy" ] || return 1
    $ADB shell input tap $xy
}

set +e   # from here on, check() reports failures

echo "== start VPN"
$ADB logcat -c
$ADB shell am start -W -n "$PKG/.MainActivity" >/dev/null
sleep 2
tap_exact "Start" || { echo "start button not found"; exit 2; }
sleep 3
tap_text "OK|ОК|Allow" >/dev/null 2>&1 || true   # VPN consent, if shown

up=1
for _ in $(seq 1 30); do
    if $ADB shell ip addr 2>/dev/null | grep -q "10.200.0.7/24"; then up=0; break; fi
    sleep 1
done
check $up "VPN interface has the CONFIG address 10.200.0.7/24"

IFACE=$($ADB shell ip -o addr | awk '/10.200.0.7\// { print $2; exit }')
MTU=$($ADB shell cat /sys/class/net/"$IFACE"/mtu 2>/dev/null | tr -d '\r')
[ "$MTU" = 1400 ]; check $? "VPN interface MTU is the CONFIG value 1400 (got ${MTU:-none})"

RULES=$($ADB shell ip route show table all 2>/dev/null | grep " dev $IFACE" || true)
echo "$RULES" | grep -q "192.168.77.0/24"; check $? "CONFIG route 192.168.77.0/24 goes through the VPN"
echo "$RULES" | grep -q "192.168.78.0/24"; check $? "CONFIG route with host bits (192.168.78.5/24) is installed as 192.168.78.0/24"
! echo "$RULES" | grep -qE "^(default|0\.0\.0\.0/0) "; check $? "no default route through the VPN (full tunnel off)"

docker exec "$SRV" ping -c 5 -W 2 10.200.0.7 >/dev/null 2>&1
check $? "server reaches the device through the tunnel"

$ADB logcat -d -s camex:* | grep -q "TUN backend: fd .* from provider"
check $? "camex used the TUN provider (logcat)"

echo "== changed profile (#49): new address, MTU and route without a restart"
sed -e 's#10.200.0.7/24#10.200.0.8/24#' -e 's#mtu=1400#mtu=1300#' \
    -e 's#192.168.77.0/24#192.168.79.0/24#' "$CTX/etc-camex/camex.conf" > "$CTX/changed.conf"
docker cp "$CTX/changed.conf" "$SRV:/etc/camex/camex.conf" >/dev/null
# A restart drops the TCP connection: the app reconnects at once and gets
# the new CONFIG with that handshake (otherwise with the next rekey)
docker restart -t 1 "$SRV" >/dev/null
moved=1
for _ in $(seq 1 40); do
    if $ADB shell ip addr 2>/dev/null | grep -q "10.200.0.8/24"; then moved=0; break; fi
    sleep 1
done
check $moved "VPN interface moved to 10.200.0.8/24"
IFACE=$($ADB shell ip -o addr | awk '/10.200.0.8\// { print $2; exit }')
MTU=$($ADB shell cat /sys/class/net/"$IFACE"/mtu 2>/dev/null | tr -d '\r')
[ "$MTU" = 1300 ]; check $? "VPN interface MTU is now 1300 (got ${MTU:-none})"
RULES=$($ADB shell ip route show table all 2>/dev/null | grep " dev $IFACE" || true)
echo "$RULES" | grep -q "192.168.79.0/24" && ! echo "$RULES" | grep -q "192.168.77.0/24"
check $? "route 192.168.79.0/24 replaced 192.168.77.0/24"
docker exec "$SRV" ping -c 5 -W 2 10.200.0.8 >/dev/null 2>&1
check $? "server reaches the device at its new address"

echo "== stop VPN"
$ADB shell am start -W -n "$PKG/.MainActivity" >/dev/null
sleep 1
tap_exact "Stop" || true
down=1
for _ in $(seq 1 15); do
    if ! $ADB shell ip addr 2>/dev/null | grep -q "10.200.0.[78]/24"; then down=0; break; fi
    sleep 1
done
check $down "VPN interface is gone after Stop"

if [ "$FAILS" -ne 0 ]; then
    echo "--- logcat (camex) ---"
    $ADB logcat -d | grep -iE "camex|vpn" | tail -30
    echo "--- server log ---"
    docker logs "$SRV" 2>&1 | tail -20
    docker inspect -f 'server state: {{.State.Status}} exit={{.State.ExitCode}}' "$SRV" 2>&1
    echo "FAIL: $FAILS check(s) failed"
    exit 2
fi
echo "PASS: Android app on $SERIAL"
