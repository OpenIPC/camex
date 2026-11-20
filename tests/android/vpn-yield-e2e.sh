#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# vpn-yield-e2e.sh — two apps with the tunnel library on one emulator: one VPN at a time, and
# no app takes it from the other by itself (dstask 23)
#
# Android holds one VPN, and VpnService.prepare() of an app that has the consent takes the VPN
# from the app that holds it. Two apps here, both with the consent (appops):
#   A  the camex app (org.openipc.camex): its BOOT_COMPLETED receiver starts the tunnel by
#      itself (startWhenFree), its START button is the user's start (start)
#   B  the library's instrumented test APK (TunnelE2eTest, steps yield-wait and yield-taken)
#
#   1. A connects (receiver), no other VPN.
#   2. B: startWhenFree() while A holds the VPN: B waits (WAITING), A keeps the VPN; A is
#      stopped: B comes up by itself.
#   3. B: start() takes the VPN and connects; A's receiver (startWhenFree) waits and takes
#      nothing; A's START (the user) takes the VPN: B waits; A's STOP: B comes back by itself.
#
# Usage: tests/android/vpn-yield-e2e.sh [API]   (API 34 by default)
#   ANDROID_IMAGE, CROSS_IMAGE, EMULATOR_IMAGE  Docker images
#   EMULATOR_DIR     the emulator, platform-tools and system images (mounted at /emu)
#   GRADLE_CACHE     Gradle's cache (mounted at /cache/gradle)
#   KVM_GROUP        the group id of /dev/kvm on the host
#
set -e

API=${1:-34}
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/vpn-yield-e2e"
ANDROID_IMAGE=${ANDROID_IMAGE:-compot/android:2026.10}
CROSS_IMAGE=${CROSS_IMAGE:-compot/cross:2026.11}
EMULATOR_IMAGE=${EMULATOR_IMAGE:-compot/android-emulator:2026.11}
EMULATOR_DIR=${EMULATOR_DIR:-$HOME/.cache/compot/toolchains/android-emulator}
GRADLE_CACHE=${GRADLE_CACHE:-$HOME/.gradle}
KVM_GROUP=${KVM_GROUP:-$(stat -c %g /dev/kvm)}
NAME=camex-vpn-yield-$$
SRV=$NAME-srv
NET=$NAME-net
A=org.openipc.camex
B=org.openipc.camex.tunnel.test
RUNNER=$B/androidx.test.runner.AndroidJUnitRunner
PSK="yield-secret"
UID_GID="$(id -u):$(id -g)"

pass=0
fail=0
ok() { pass=$((pass + 1)); echo "PASS: $*"; }
bad() { fail=$((fail + 1)); echo "FAIL: $*"; }

cleanup() {
    docker rm -f "$NAME" "$SRV" >/dev/null 2>&1 || true
    docker network rm "$NET" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

rm -rf "$OUT"
mkdir -p "$OUT/w"

echo "== camex (static), the camex app and the test APK (x86_64)"
docker run --rm --pull never -u "$UID_GID" -v "$ROOT:/src" -w /src "$CROSS_IMAGE" sh -c '
    rm -rf build/vpn-yield-e2e/camex && mkdir -p build/vpn-yield-e2e/camex &&
    cp -a Makefile src build/vpn-yield-e2e/camex/ && cd build/vpn-yield-e2e/camex && make -s LDFLAGS=-static' \
    > "$OUT/camex.log" 2>&1 || { tail -30 "$OUT/camex.log"; exit 1; }
cp "$OUT/camex/camex" "$OUT/w/"
KEYS=$("$OUT/w/camex" --genkey)
printf '%s\n' "$KEYS" | sed -n 's/^server_key=/server_key=/p' > "$OUT/w/server.key"
PUBKEY=$(printf '%s\n' "$KEYS" | sed -n 's/^public_key=//p')
docker run --rm --pull never -u "$UID_GID" -v "$ROOT:/src" -v "$GRADLE_CACHE:/cache/gradle" \
    -w /src/android "$ANDROID_IMAGE" gradle --no-daemon -q -PcamexAbis=x86_64 \
    :tunnel:assembleDebugAndroidTest :app:assembleDebug > "$OUT/gradle.log" 2>&1 || { tail -30 "$OUT/gradle.log"; exit 1; }
cp "$ROOT/android/tunnel/build/outputs/apk/androidTest/debug/tunnel-debug-androidTest.apk" "$OUT/w/b.apk"
cp "$(ls "$ROOT"/android/app/build/outputs/apk/debug/*.apk | head -1)" "$OUT/w/a.apk"

cat > "$OUT/w/avd.sh" <<EOS
#!/bin/sh
export ANDROID_SDK_ROOT=/emu ANDROID_HOME=/emu ANDROID_AVD_HOME=/w/.emu/avd HOME=/w/.emu/home
D=\$ANDROID_AVD_HOME/e2e.avd
mkdir -p \$D \$HOME
printf 'avd.ini.encoding=UTF-8\npath=%s\ntarget=android-$API\n' \$D > \$ANDROID_AVD_HOME/e2e.ini
printf 'abi.type=x86_64\nhw.cpu.arch=x86_64\nimage.sysdir.1=system-images/android-$API/default/x86_64/\ntag.id=default\nhw.ramSize=2048\ndisk.dataPartition.size=2G\nhw.lcd.width=720\nhw.lcd.height=1280\nhw.lcd.density=320\nhw.gpu.enabled=yes\nhw.gpu.mode=swiftshader_indirect\n' > \$D/config.ini
/emu/emulator/emulator -avd e2e -no-window -no-audio -no-boot-anim -no-snapshot -gpu swiftshader_indirect -accel on > /w/emulator.log 2>&1 &
/emu/platform-tools/adb wait-for-device
until [ "\$(/emu/platform-tools/adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]; do sleep 2; done
echo BOOTED
sleep 100000
EOS

echo "== emulator (API $API) and a camex server"
docker network create "$NET" >/dev/null
docker run -d --name "$NAME" --network "$NET" --pull never --device /dev/kvm --group-add "$KVM_GROUP" \
    -u "$UID_GID" -v "$EMULATOR_DIR:/emu:ro" -v "$OUT/w:/w" "$EMULATOR_IMAGE" sh /w/avd.sh >/dev/null
docker run -d --name "$SRV" --network "$NET" --pull never --cap-add NET_ADMIN --device /dev/net/tun \
    -e CAMEX_POOL_HOLD=0 -v "$OUT/w:/w" "$CROSS_IMAGE" sh -c "/w/camex --mode server --port 5800 \
    --transport udp --local-cidr 10.200.0.1/24 --server-key-file /w/server.key --psk $PSK \
    > /w/server.log 2>&1" >/dev/null
SRV_IP=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$SRV")
i=0
until docker logs "$NAME" 2>&1 | grep -q BOOTED; do
    i=$((i + 1))
    if [ $i -ge 120 ] || grep -q 'requires hardware acceleration' "$OUT/w/emulator.log" 2>/dev/null; then
        echo "the emulator did not boot:"; tail -5 "$OUT/w/emulator.log"; exit 1
    fi
    sleep 5
done
ADB="docker exec $NAME /emu/platform-tools/adb"
$ADB root >/dev/null 2>&1 || true    # BOOT_COMPLETED to A's receiver needs root
sleep 3
$ADB wait-for-device
$ADB install -r -t /w/a.apk >/dev/null
$ADB install -r -t /w/b.apk >/dev/null
if [ "$API" -ge 33 ]; then
    $ADB shell pm grant $A android.permission.POST_NOTIFICATIONS
    $ADB shell pm grant $B android.permission.POST_NOTIFICATIONS
fi
$ADB shell appops set $A ACTIVATE_VPN allow
$ADB shell appops set $B ACTIVATE_VPN allow

# A's settings, as its settings screen saves them
cat > "$OUT/w/camex.xml" <<EOF
<?xml version='1.0' encoding='utf-8' standalone='yes' ?>
<map>
    <string name="server_host">$SRV_IP</string>
    <int name="server_port" value="5800" />
    <string name="name">appA</string>
    <string name="transport">udp</string>
    <string name="server_pubkey">$PUBKEY</string>
    <string name="psk">$PSK</string>
    <boolean name="start_on_boot" value="true" />
</map>
EOF
$ADB push /w/camex.xml /data/local/tmp/camex.xml >/dev/null
$ADB shell "chmod 644 /data/local/tmp/camex.xml; run-as $A sh -c 'mkdir -p shared_prefs && cp /data/local/tmp/camex.xml shared_prefs/camex.xml'"

a_boot() { $ADB shell am broadcast -a android.intent.action.BOOT_COMPLETED -n $A/.BootReceiver >/dev/null; }
# a button of A's screen (START, STOP), from the UI dump
a_tap() {
    local xy _
    $ADB shell am start -n $A/.MainActivity >/dev/null 2>&1
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        sleep 1
        $ADB shell uiautomator dump /data/local/tmp/ui.xml >/dev/null 2>&1
        xy=$($ADB shell cat /data/local/tmp/ui.xml | tr '>' '\n' | grep "text=\"$1\"" | head -1 |
            sed -nE 's/.*bounds="\[([0-9]+),([0-9]+)\]\[([0-9]+),([0-9]+)\]".*/\1 \2 \3 \4/p' |
            awk '{ print int(($1 + $3) / 2), int(($2 + $4) / 2) }')
        [ -n "$xy" ] && { $ADB shell input tap $xy; return 0; }
    done
    echo "no $1 on A's screen"
    return 1
}
a_board() {  # the text of A's TUNNEL row
    $ADB shell am start -n $A/.MainActivity >/dev/null 2>&1
    sleep 2
    $ADB shell uiautomator dump /data/local/tmp/ui.xml >/dev/null 2>&1
    $ADB shell cat /data/local/tmp/ui.xml | tr '>' '\n' | grep -oE 'text="(CONNECTED|CONNECTING|WAITING|STOPPED|VPN CONSENT)[^"]*"' | head -1
}
client_up() {  # client_up <ID>: the server lists the client
    docker exec "$SRV" /w/camex --status 2>/dev/null | awk 'NR > 2 { print $1 }' | grep -qx "$1"
}
switches() { $ADB logcat -d 2>/dev/null | tr -d '\r' | grep -c "Switched from" || true; }
e2e_log() { $ADB logcat -d -s CAMEX_E2E 2>/dev/null | tr -d '\r'; }
wait_log() {  # wait_log <seconds> <text>: in the test's log
    local _
    for _ in $(seq 1 "$1"); do e2e_log | grep -q "$2" && return 0; sleep 1; done
    return 1
}
instrument() {
    $ADB shell am instrument -w -e step "$1" -e server "$SRV_IP:5800" -e pubkey "$PUBKEY" \
        -e psk "$PSK" -e transport UDP -e name appB -e class "org.openipc.camex.tunnel.TunnelE2eTest#$2" \
        $RUNNER 2>&1 | tr -d '\r'
}

echo "== 1. A connects by itself (its receiver), no other VPN"
$ADB logcat -c
a_boot
i=0; until client_up appA; do i=$((i + 1)); [ $i -lt 60 ] || break; sleep 1; done
client_up appA && ok "A connected (startWhenFree with no other VPN)" || bad "A did not connect"

echo "== 2. B's startWhenFree() while A holds the VPN"
$ADB logcat -c
instrument yield-wait yieldWait > "$OUT/yield-wait.log" &
RUN=$!
if wait_log 30 "waiting OK"; then
    ok "B waits (WAITING): another app's VPN is on"
    sleep 3
    [ "$(switches)" = 0 ] && ok "A keeps the VPN: nothing switched" || bad "the VPN switched while B waited"
    client_up appA && $ADB shell 'ip -4 -o addr' | grep -q ' tun' && ok "A's tunnel is still up" \
        || bad "A's tunnel went"
else
    bad "B did not wait"
fi
a_tap STOP || true
wait_log 60 "free OK" && ok "A stopped: B came up by itself" || bad "B did not come up after A stopped"
wait $RUN || true
grep -q '^OK (' "$OUT/yield-wait.log" && ok "the yield-wait step passed" || { bad "the yield-wait step"; tail -20 "$OUT/yield-wait.log"; }

echo "== 3. B's start() takes the VPN; A by itself waits, A's START takes it back"
$ADB logcat -c
instrument yield-taken yieldTaken > "$OUT/yield-taken.log" &
RUN=$!
if wait_log 60 "holding OK"; then
    ok "B connected (start)"
    sleep 3    # B's VPN is on the other apps' list a moment after it is up (Android 7)
    n=$(switches)
    a_boot
    sleep 6
    [ "$(switches)" = "$n" ] && ok "A's receiver (startWhenFree) took nothing" || bad "A's receiver took the VPN"
    case "$(a_board)" in
        *WAITING*) ok "A's board shows WAITING FOR THE VPN";;
        *) bad "A's board: $(a_board)";;
    esac
    a_tap START || true
    wait_log 60 "taken OK" && ok "A's START took the VPN: B waits" || bad "B did not wait after A's START"
    i=0; until client_up appA; do i=$((i + 1)); [ $i -lt 30 ] || break; sleep 1; done
    sleep 3
    [ "$(switches)" -le "$((n + 1))" ] && ok "B took nothing back while it waited" \
        || bad "the VPN switched more than once"
    a_tap STOP || true
    wait_log 60 "back OK" && ok "A stopped: B came back by itself" || bad "B did not come back"
else
    bad "B did not connect"
fi
wait $RUN || true
grep -q '^OK (' "$OUT/yield-taken.log" && ok "the yield-taken step passed" || { bad "the yield-taken step"; tail -20 "$OUT/yield-taken.log"; }

$ADB logcat -d > "$OUT/logcat.txt" 2>&1 || true
grep -q 'FATAL EXCEPTION' "$OUT/logcat.txt" && bad "a crash in logcat ($OUT/logcat.txt)" || ok "no crash"
echo "== $pass passed, $fail failed (API $API)"
[ "$fail" -eq 0 ]
