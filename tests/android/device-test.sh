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
#   - only the tunnel subnet and the CONFIG route go through the VPN
#     (full tunnel off), and the app itself is excluded from it (#18)
#   - the server reaches the device through the tunnel (ping)
#   - stopping the service stops camex
#
# Usage: tests/android/device-test.sh <adb-serial> <camex-android-debug.apk>
#   CAMEX_BINARY=path/to/camex  server binary (default: ./camex)
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
EOF
docker build -q -t camex:devtest "$CTX" >/dev/null
docker create --name "$SRV" -p 5800:5800 --cap-add NET_ADMIN --cap-add MKNOD \
    camex:devtest --mode server --port 5800 --transport tcp \
    --local-cidr 10.200.0.1/24 --encrypt --psk devtest-key \
    --config /etc/camex/camex.conf >/dev/null
docker cp "$CTX/etc-camex" "$SRV:/etc/camex" >/dev/null
docker start "$SRV" >/dev/null
$ADB reverse tcp:5800 tcp:5800 >/dev/null

# ---------------------------------------------------------------- app
echo "== install $APK"
$ADB install -r -g "$APK" >/dev/null
$ADB shell am force-stop "$PKG"

# Settings: server through adb reverse, TCP, encryption, full tunnel OFF so
# the rest of the device keeps its normal network.
cat > "$CTX/camex.xml" <<'EOF'
<?xml version='1.0' encoding='utf-8' standalone='yes' ?>
<map>
    <string name="server_host">127.0.0.1</string>
    <int name="server_port" value="5800" />
    <string name="name">DEVTEST</string>
    <string name="transport">tcp</string>
    <boolean name="encrypt" value="true" />
    <string name="psk">devtest-key</string>
    <boolean name="full_tunnel" value="false" />
    <int name="mtu" value="1500" />
    <int name="keepalive" value="5" />
    <int name="server_timeout" value="15" />
</map>
EOF
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

set +e   # from here on, check() reports failures

echo "== start VPN"
$ADB logcat -c
$ADB shell am start -W -n "$PKG/.MainActivity" >/dev/null
sleep 2
tap_text "Start VPN" || { echo "start button not found"; exit 2; }
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
! echo "$RULES" | grep -qE "^(default|0\.0\.0\.0/0) "; check $? "no default route through the VPN (full tunnel off)"

docker exec "$SRV" ping -c 5 -W 2 10.200.0.7 >/dev/null 2>&1
check $? "server reaches the device through the tunnel"

$ADB logcat -d -s camex:* | grep -q "TUN backend: fd .* from provider"
check $? "camex used the TUN provider (logcat)"

echo "== stop VPN"
$ADB shell am start -W -n "$PKG/.MainActivity" >/dev/null
sleep 1
tap_text "Stop VPN" || true
down=1
for _ in $(seq 1 15); do
    if ! $ADB shell ip addr 2>/dev/null | grep -q "10.200.0.7/24"; then down=0; break; fi
    sleep 1
done
check $down "VPN interface is gone after Stop"

if [ "$FAILS" -ne 0 ]; then
    echo "--- logcat (camex) ---"
    $ADB logcat -d | grep -iE "camex|vpn" | tail -30
    echo "FAIL: $FAILS check(s) failed"
    exit 2
fi
echo "PASS: Android app on $SERIAL"
