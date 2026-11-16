#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# tunnel-e2e.sh — end-to-end test of the tunnel library (android/tunnel, #170) on the
# Android emulator, as an app that builds camex in uses it
#
#   1. builds camex (static, for the containers) and the library's instrumented test APK
#      (x86_64);
#   2. boots an x86_64 emulator in Docker; a camex server and a Linux client "peer" run in
#      containers of their own on the same Docker network; the peer answers "peer-ok" on
#      TCP 9000 in its network namespace, so it is reached only through the tunnel;
#   3. the server gives the test app ([client app] in its config) routes: the
#      Docker network itself (the app's own server connection would loop into the tunnel if
#      camex's socket were not protected), a default route (must be left out) and a route
#      with host bits set (must be installed as its network);
#   4. runs the instrumented test (TunnelE2eTest) step by step:
#      - consent: without the VPN consent (appops) start() gives NEEDS_CONSENT, no crash;
#      - bad:     bad values give FAILED with the field's name;
#      - run:     with the consent (adb shell appops set <pkg> ACTIVATE_VPN allow; Android 5.0:
#                 the system dialog, its OK tapped): CONNECTED
#                 with the address, the routes as above and no default route, TCP out to
#                 the peer through the tunnel, TCP in from the peer to the app's listening
#                 socket 0.0.0.0:8080; new values while running (camex again in-process);
#                 the server stopped (CONNECTING) and started (CONNECTED); stop() and the
#                 VPN gone; start() after stop(); stop() right after start().
#
# Usage: tests/android/tunnel-e2e.sh [API]   (API 21 by default; 34 needs the android-34 image)
#   ANDROID_IMAGE, CROSS_IMAGE, EMULATOR_IMAGE, PEER_IMAGE  Docker images (see below)
#   EMULATOR_DIR     the emulator, platform-tools and system images (mounted at /emu)
#   GRADLE_CACHE     Gradle's cache (mounted at /cache/gradle)
#   KVM_GROUP        the group id of /dev/kvm
#   TRANSPORT        UDP (default) or TCP
#
set -e

API=${1:-21}
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/tunnel-e2e"
ANDROID_IMAGE=${ANDROID_IMAGE:-compot/android:2026.10}
CROSS_IMAGE=${CROSS_IMAGE:-compot/cross:2026.11}
EMULATOR_IMAGE=${EMULATOR_IMAGE:-compot/android-emulator:2026.11}
PEER_IMAGE=${PEER_IMAGE:-alpine:3.22}
EMULATOR_DIR=${EMULATOR_DIR:-$HOME/.cache/compot/toolchains/android-emulator}
GRADLE_CACHE=${GRADLE_CACHE:-$HOME/.gradle}
KVM_GROUP=${KVM_GROUP:-$(stat -c %g /dev/kvm)}
TRANSPORT=${TRANSPORT:-UDP}
NAME=camex-tunnel-e2e-$$
SRV=$NAME-srv
PEER=$NAME-peer
NC=$NAME-nc
NET=$NAME-net
PKG=org.openipc.camex.tunnel.test
RUNNER=$PKG/androidx.test.runner.AndroidJUnitRunner
PSK="e2e-secret"
UID_GID="$(id -u):$(id -g)"

pass=0
fail=0
ok() { pass=$((pass + 1)); echo "PASS: $*"; }
bad() { fail=$((fail + 1)); echo "FAIL: $*"; }

cleanup() {
    docker rm -f "$NAME" "$NC" "$PEER" "$SRV" >/dev/null 2>&1 || true
    docker network rm "$NET" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

rm -rf "$OUT"
mkdir -p "$OUT/w"

echo "== camex (static)"
docker run --rm --pull never -u "$UID_GID" -v "$ROOT:/src" -w /src "$CROSS_IMAGE" sh -c '
    rm -rf build/tunnel-e2e/camex && mkdir -p build/tunnel-e2e/camex &&
    cp -a Makefile src build/tunnel-e2e/camex/ && cd build/tunnel-e2e/camex && make -s LDFLAGS=-static' \
    > "$OUT/camex.log" 2>&1 || { tail -30 "$OUT/camex.log"; exit 1; }
cp "$OUT/camex/camex" "$OUT/w/"
KEYS=$("$OUT/w/camex" --genkey)
printf '%s\n' "$KEYS" | sed -n 's/^server_key=/server_key=/p' > "$OUT/w/server.key"
PUBKEY=$(printf '%s\n' "$KEYS" | sed -n 's/^public_key=//p')

echo "== instrumented test APK (x86_64)"
docker run --rm --pull never -u "$UID_GID" -v "$ROOT:/src" -v "$GRADLE_CACHE:/cache/gradle" \
    -w /src/android "$ANDROID_IMAGE" gradle --no-daemon -q -PcamexAbis=x86_64 \
    :tunnel:assembleDebugAndroidTest > "$OUT/gradle.log" 2>&1 || { tail -30 "$OUT/gradle.log"; exit 1; }
cp "$ROOT/android/tunnel/build/outputs/apk/androidTest/debug/tunnel-debug-androidTest.apk" "$OUT/w/test.apk"

cat > "$OUT/w/avd.sh" <<EOS
#!/bin/sh
export ANDROID_SDK_ROOT=/emu ANDROID_HOME=/emu ANDROID_AVD_HOME=/w/.emu/avd HOME=/w/.emu/home
A=\$ANDROID_AVD_HOME/e2e.avd
mkdir -p \$A \$HOME
printf 'avd.ini.encoding=UTF-8\npath=%s\ntarget=android-$API\n' \$A > \$ANDROID_AVD_HOME/e2e.ini
printf 'abi.type=x86_64\nhw.cpu.arch=x86_64\nimage.sysdir.1=system-images/android-$API/default/x86_64/\ntag.id=default\nhw.ramSize=2048\ndisk.dataPartition.size=2G\nhw.lcd.width=720\nhw.lcd.height=1280\nhw.lcd.density=320\nhw.gpu.enabled=yes\nhw.gpu.mode=swiftshader_indirect\n' > \$A/config.ini
/emu/emulator/emulator -avd e2e -no-window -no-audio -no-boot-anim -no-snapshot -gpu swiftshader_indirect -accel on > /w/emulator.log 2>&1 &
/emu/platform-tools/adb wait-for-device
until [ "\$(/emu/platform-tools/adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]; do sleep 2; done
echo BOOTED
sleep 100000
EOS

echo "== emulator (API $API)"
# a network of its own: the default bridge overlaps some lab networks
docker network create "$NET" >/dev/null
SUBNET=$(docker network inspect -f '{{range .IPAM.Config}}{{.Subnet}}{{end}}' "$NET")
docker run -d --name "$NAME" --network "$NET" --pull never --device /dev/kvm --group-add "$KVM_GROUP" \
    -u "$UID_GID" -v "$EMULATOR_DIR:/emu:ro" -v "$OUT/w:/w" "$EMULATOR_IMAGE" sh /w/avd.sh >/dev/null

# the server: the app's profile ([client app]) with the Docker network as a route (the app
# reaches the server there: only a protected socket gets out), a default route and a route with
# host bits set; the address comes from the auto pool
cat > "$OUT/w/camex.conf" <<EOF
[server]
[client app]
route_cidr=$SUBNET
route_cidr=0.0.0.0/0
route_cidr=192.0.2.77/24
EOF
docker run -d --name "$SRV" --network "$NET" --pull never --cap-add NET_ADMIN --device /dev/net/tun \
    -e CAMEX_POOL_HOLD=0 -v "$OUT/w:/w" "$CROSS_IMAGE" sh -c "/w/camex --mode server --port 5800 \
    --transport $(echo "$TRANSPORT" | tr A-Z a-z) --local-cidr 10.200.0.1/24 \
    --server-key-file /w/server.key --psk $PSK --config /w/camex.conf > /w/server.log 2>&1" >/dev/null
SRV_IP=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$SRV")
sleep 1
docker run -d --name "$PEER" --network "$NET" --pull never --cap-add NET_ADMIN --device /dev/net/tun \
    -v "$OUT/w:/w" "$CROSS_IMAGE" sh -c "/w/camex --mode client --auto --name peer --server-host $SRV_IP \
    --port 5800 --transport $(echo "$TRANSPORT" | tr A-Z a-z) --server-pubkey $PUBKEY --psk $PSK \
    > /w/peer.log 2>&1" >/dev/null
i=0
# the peer's tunnel address: the LOCAL /32 of 10.200. in its routing table (no ip tool there)
until PEER_IP=$(docker exec "$PEER" cat /proc/net/fib_trie 2>/dev/null |
        awk '/\|-- 10\.200\./ { a = $2 } /\/32 host LOCAL/ && a { print a; exit }') && [ -n "$PEER_IP" ]; do
    i=$((i + 1)); [ $i -lt 30 ] || { echo "the peer got no address"; tail -20 "$OUT/w/peer.log"; exit 1; }
    sleep 1
done
# "peer-ok" on TCP 9000 in the peer's network namespace
docker run -d --name "$NC" --net "container:$PEER" --pull never "$PEER_IMAGE" \
    sh -c 'while true; do echo peer-ok | nc -l -p 9000; done' >/dev/null
echo "server: $SRV_IP:5800 ($TRANSPORT), peer: $PEER_IP, Docker network: $SUBNET"

i=0
until docker logs "$NAME" 2>&1 | grep -q BOOTED; do
    i=$((i + 1))
    if [ $i -ge 120 ] || grep -q 'requires hardware acceleration' "$OUT/w/emulator.log" 2>/dev/null; then
        echo "the emulator did not boot:"; tail -5 "$OUT/w/emulator.log"; exit 1
    fi
    sleep 5
done
ADB="docker exec $NAME /emu/platform-tools/adb"
$ADB uninstall $PKG >/dev/null 2>&1 || true
$ADB install -r -t /w/test.apk >/dev/null
[ "$API" -lt 33 ] || $ADB shell pm grant $PKG android.permission.POST_NOTIFICATIONS
instrument() {
    local step=$1
    shift
    $ADB shell am instrument -w -e step "$step" -e server "$SRV_IP:5800" -e pubkey "$PUBKEY" \
        -e psk "$PSK" -e transport "$TRANSPORT" "$@" $RUNNER 2>&1 | tr -d '\r'
}
passed() { grep -q '^OK (' <<<"$1"; }

# The VPN consent over adb, as on an unattended device; Android 5.0 has no appops
# ACTIVATE_VPN (5.1 and later do): there the system dialog is shown (ConsentActivity of the
# test APK) and its OK tapped
consent() {
    local xy _
    if ! $ADB shell appops set $PKG ACTIVATE_VPN "$1" 2>&1 | grep -q 'Unknown operation'; then
        return 0
    fi
    [ "$1" = allow ] || return 0     # a fresh install has no consent
    instrument dialog >/dev/null
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        sleep 1
        $ADB shell uiautomator dump /data/local/tmp/ui.xml >/dev/null 2>&1
        xy=$($ADB shell cat /data/local/tmp/ui.xml | tr '>' '\n' |
            grep 'resource-id="android:id/button1"' | head -1 |
            sed -nE 's/.*bounds="\[([0-9]+),([0-9]+)\]\[([0-9]+),([0-9]+)\]".*/\1 \2 \3 \4/p' |
            awk '{ print int(($1 + $3) / 2), int(($2 + $4) / 2) }')
        [ -n "$xy" ] && { $ADB shell input tap $xy; sleep 2; return 0; }
    done
    echo "no consent dialog to confirm"
}

# a VPN consent left from an earlier run would hide the case
consent ignore
R=$(instrument consent)
if passed "$R"; then ok "no consent: NEEDS_CONSENT, no crash"; else bad "no consent"; echo "$R" | tail -20; fi

R=$(instrument bad)
if passed "$R"; then ok "bad values: FAILED with the field's name"; else bad "bad values"; echo "$R" | tail -20; fi

consent allow
$ADB logcat -c
instrument run -e name app -e peer "$PEER_IP:9000" -e listen 8080 \
    -e routes "10.200.0.0/24,$SUBNET,192.0.2.0/24" > "$OUT/run.log" 2>&1 &
RUN=$!
# in: as soon as the app listens in the tunnel, the peer connects to it
got=
APP_IP=
for i in $(seq 1 120); do
    APP_IP=$($ADB logcat -d -s CAMEX_E2E 2>/dev/null | sed -n 's/.*listening \([0-9.]*\):8080.*/\1/p' | tail -1)
    if [ -n "$APP_IP" ]; then
        for _ in $(seq 1 10); do
            got=$(docker run --rm --net "container:$PEER" --pull never "$PEER_IMAGE" \
                sh -c "nc -w 5 $APP_IP 8080 </dev/null" 2>/dev/null | head -1)
            [ "$got" = app-ok ] && break
            sleep 1
        done
        break
    fi
    kill -0 $RUN 2>/dev/null || break
    sleep 1
done
# the server goes away while the app is connected, and comes back
lost=
for i in $(seq 1 300); do
    LOG=$($ADB logcat -d -s CAMEX_E2E 2>/dev/null)
    if grep -q "server-loss ready" <<<"$LOG"; then
        if [ -z "$lost" ]; then docker stop -t 1 "$SRV" >/dev/null; lost=1; fi
        if grep -q "lost OK" <<<"$LOG"; then docker start "$SRV" >/dev/null; break; fi
    fi
    kill -0 $RUN 2>/dev/null || break
    sleep 1
done
wait $RUN || true
$ADB logcat -d > "$OUT/logcat.txt" 2>&1 || true
L=$(grep CAMEX_E2E "$OUT/logcat.txt" || true)
if [ -n "$APP_IP" ] && echo "$L" | grep -q "connected $APP_IP"; then
    ok "CONNECTED with $APP_IP from the server's pool"
else
    bad "not CONNECTED"
fi
echo "$L" | grep -q "routes " && echo "$L" | grep "routes " | sed 's/.*CAMEX_E2E: /  /'
echo "$L" | grep -q "outgoing OK" && ok "TCP to the peer $PEER_IP through the tunnel (no default route, routes as given)" \
    || bad "TCP to the peer through the tunnel"
[ "$got" = app-ok ] && ok "TCP from the peer to the app's 0.0.0.0:8080 through the tunnel" \
    || bad "TCP from the peer to the app's listening socket"
echo "$L" | grep -q "restart OK" && ok "new values while running: camex again in-process, CONNECTED, peer reached" \
    || bad "restart with new values"
echo "$L" | grep -q "lost OK" && ok "server stopped: CONNECTING" || bad "no CONNECTING when the server went away"
echo "$L" | grep -q "back OK" && ok "server back: CONNECTED, peer reached" || bad "not CONNECTED after the server came back"
echo "$L" | grep -q "stop OK" && ok "stop(): STOPPED and the VPN gone" || bad "stop()"
echo "$L" | grep -q "start after stop OK" && ok "start() after stop(): CONNECTED" || bad "start() after stop()"
echo "$L" | grep -q "quick stop OK" && ok "stop() right after start(): stays STOPPED" || bad "stop() right after start()"
if passed "$(cat "$OUT/run.log")"; then ok "the run step passed"; else bad "the run step"; tail -30 "$OUT/run.log"; fi
grep -q 'FATAL EXCEPTION' "$OUT/logcat.txt" && bad "a crash in logcat ($OUT/logcat.txt)" || ok "no crash"
# the psk never reaches logcat (the test runner logs its own arguments: not the library)
grep -v -E 'AndroidJUnitRunner|TestRunner' "$OUT/logcat.txt" | grep -q "$PSK" &&
    bad "the psk is in logcat" || ok "the psk is not in logcat"

echo "== $pass passed, $fail failed (API $API, $TRANSPORT)"
[ "$fail" -eq 0 ]
