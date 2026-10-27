#!/bin/sh
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# run-vm.sh — runs inside a Debian/Ubuntu container (see
# docker/test-kmod-vm.sh): builds camex.ko against the distro kernel
# headers, boots the matching kernel in QEMU with a busybox initramfs and
# runs kmodtest in the guest. Expects the sources in /k/src and /k/kmod.
#
# With a static camex binary in /k/camex, the guest then runs the camex
# client over /dev/camex against a camex server here, reached through QEMU
# user networking (10.0.2.2), as latebra's VM test does with vtund:
# address and route from CONFIG, IPv6 off on camex, traffic both ways;
# after SIGTERM camex stays (camex.ko), down, without address and route.
#
set -e
export DEBIAN_FRONTEND=noninteractive
. /etc/os-release
FLAVOUR=generic
[ "$ID" = debian ] && FLAVOUR=amd64
apt-get update -qq >/dev/null
apt-get install -y -qq "linux-headers-$FLAVOUR" qemu-system-x86 busybox-static \
    gcc make cpio iproute2 iputils-ping zstd xz-utils >/dev/null 2>&1
K=$(ls -d /usr/src/linux-headers-*-"$FLAVOUR" | head -1)
KVER=${K#/usr/src/linux-headers-}
echo "kernel: $KVER"
cd /tmp
apt-get download -qq "linux-image-unsigned-$KVER" >/dev/null 2>&1 || \
    apt-get download -qq "linux-image-$KVER" >/dev/null 2>&1
dpkg -x linux-image-*"$KVER"*.deb img
VMLINUZ=$(ls img/boot/vmlinuz-* | head -1)
CLIENT=
[ -x /k/camex ] && CLIENT=1

gcc -static -O1 -pthread -o kmodtest /k/kmod/kmodtest.c
cp -r /k/src /tmp/m
make -s -C "$K" M=/tmp/m modules 2>&1 | grep -v "compiler differs\|built by\|You are using\|BTF" || true
# A module for another layout of struct net_device (#134): two pointers
# before netdev_ops, as CONFIG_WIRELESS_EXT adds them (#133). The headers of
# this throwaway container are changed for it, after the real build
for h in $(find /usr/src -path '*/include/linux/netdevice.h'); do
    sed -i '0,/^[[:space:]]*const struct net_device_ops[[:space:]]*\*netdev_ops;/s//\tvoid *camex_layout_test[2];\n&/' "$h"
done
grep -rq camex_layout_test /usr/src/*/include/linux/netdevice.h ||
    { echo "FAIL: kmod: no netdev_ops in netdevice.h to shift"; exit 1; }
cp -r /k/src /tmp/mbad
make -s -C "$K" M=/tmp/mbad modules 2>&1 | grep -v "compiler differs\|built by\|You are using\|BTF" || true

mkdir -p rootfs/bin rootfs/dev rootfs/proc rootfs/sys rootfs/mod rootfs/tmp
cp /bin/busybox rootfs/bin/busybox
for a in sh mount insmod rmmod dmesg ip chmod sleep poweroff cat grep sed ping kill; do
    ln -s busybox rootfs/bin/$a
done
cp /tmp/m/camex.ko kmodtest rootfs/
cp /tmp/mbad/camex.ko rootfs/camex-bad.ko
printf '' > rootfs/client.env
if [ -n "$CLIENT" ]; then
    # The guest's network driver: virtio_net built in or a module (with deps)
    apt-get download -qq "linux-modules-$KVER" >/dev/null 2>&1 &&
        dpkg -x linux-modules-*"$KVER"*.deb img
    MODS=img/lib/modules/$KVER
    if [ -f "$MODS/modules.builtin" ] &&
        ! grep -q "/virtio_net.ko" "$MODS/modules.builtin"; then
        for m in failover net_failover virtio_net; do
            f=$(grep -o "[^ ]*/$m\.ko[^:]*" "$MODS/modules.dep" | head -1)
            [ -n "$f" ] || continue
            case "$f" in    # busybox insmod reads plain .ko only
                *.zst) zstd -dq "$MODS/$f" -o "rootfs/mod/$m.ko" ;;
                *.xz)  xz -dc "$MODS/$f" > "rootfs/mod/$m.ko" ;;
                *)     cp "$MODS/$f" "rootfs/mod/$m.ko" ;;
            esac
        done
    fi
    # The server here: the guest reaches it as 10.0.2.2:5800/udp
    mkdir -p /dev/net
    [ -c /dev/net/tun ] || mknod /dev/net/tun c 10 200
    KEYS=$(/k/camex --genkey)
    echo "$KEYS" | sed -n 's/^server_key=/server_key=/p' > /tmp/server.key
    PUB=$(echo "$KEYS" | sed -n 's/^public_key=//p')
    CAMEX_POOL_HOLD=0 /k/camex --mode server --port 5800 --local-cidr 10.200.0.1/24 \
        --server-key-file /tmp/server.key --psk vm-test > /tmp/server.log 2>&1 &
    cp /k/camex rootfs/camex
    echo "PUB=$PUB" > rootfs/client.env
fi
cat > rootfs/init <<'INIT'
#!/bin/sh
mount -t devtmpfs dev /dev
mount -t proc proc /proc
mount -t sysfs sys /sys
# Kernel messages in dmesg only: on the console they would split the lines;
# the first line out starts after the BIOS's screen codes
dmesg -n 1
echo
# The module built for another layout must refuse to load, not oops (#134)
if insmod /camex-bad.ko 2>/dev/null; then
    echo "FAIL: kmod: a camex.ko for another struct net_device loaded"
    rmmod camex
elif dmesg | grep -q "camex: built for another kernel configuration"; then
    echo "ok: kmod: a camex.ko for another struct net_device refuses to load"
else
    echo "FAIL: kmod: the other layout's camex.ko failed without its message"
fi
dmesg | grep -qiE "Oops|BUG:|Unable to handle" &&
    echo "FAIL: kmod: the kernel oopsed" || echo "ok: kmod: no oops"
insmod /camex.ko
sleep 1
/kmodtest
. /client.env
if [ -n "$PUB" ]; then
    for f in /mod/failover.ko /mod/net_failover.ko /mod/virtio_net.ko; do
        [ -f "$f" ] && insmod "$f"
    done
    ip link set lo up
    ip link set eth0 up
    ip addr add 10.0.2.15/24 dev eth0
    ip route add default via 10.0.2.2
    # kmodtest left 10.9.0.1/24 on camex: start from a clean interface,
    # IPv6 on as camex.ko creates it
    ip addr flush dev camex
    ip link set camex down
    V6=/proc/sys/net/ipv6/conf/camex/disable_ipv6
    [ -f $V6 ] && echo 0 > $V6
    /camex --mode client --auto --name VMTEST --server-host 10.0.2.2 --port 5800 \
        --server-pubkey "$PUB" --psk vm-test --tun-dev /dev/camex --keepalive 2 \
        > /tmp/log 2>&1 &
    cx=$!
    i=0
    until ip -4 addr show camex | grep -q "inet 10.200.0." || [ $i -gt 60 ]; do
        sleep 1; i=$((i + 1))
    done
    ip -4 addr show camex | grep -q "inet 10.200.0.2/24" &&
        echo "ok: client: camex has 10.200.0.2/24 from CONFIG" ||
        echo "FAIL: client: no address on camex"
    ip route | grep -q "10.200.0.0/24 dev camex" &&
        echo "ok: client: route 10.200.0.0/24 dev camex" || echo "FAIL: client: no route"
    if [ -f $V6 ]; then
        [ "$(cat $V6)" = 1 ] &&
            echo "ok: client: IPv6 off on camex" || echo "FAIL: client: IPv6 on camex"
    else
        echo "ok: client: no IPv6 in this kernel"
    fi
    ping -c 3 -W 2 10.200.0.1 >/dev/null &&
        echo "ok: client: guest pings the server through camex.ko" ||
        echo "FAIL: client: guest cannot ping the server"
    echo "client-up"
    sleep 6         # the server pings the guest meanwhile
    kill -TERM $cx
    i=0
    while kill -0 $cx 2>/dev/null && [ $i -lt 10 ]; do sleep 1; i=$((i + 1)); done
    kill -0 $cx 2>/dev/null && echo "FAIL: client: still running after SIGTERM" ||
        echo "ok: client: exits on SIGTERM"
    ip link show camex >/dev/null 2>&1 && ! ip link show camex | grep -q "[<,]UP[,>]" &&
        echo "ok: client: camex stays after SIGTERM, down" ||
        echo "FAIL: client: camex gone or still up"
    ip -4 addr show camex | grep -q inet && echo "FAIL: client: address left on camex" ||
        echo "ok: client: no address on camex after SIGTERM"
    ip route | grep -q "dev camex" && echo "FAIL: client: route left on camex" ||
        echo "ok: client: no route on camex after SIGTERM"
    if [ -f $V6 ]; then
        [ "$(cat $V6)" = 0 ] && echo "ok: client: IPv6 setting of camex put back" ||
            echo "FAIL: client: IPv6 left off on camex"
    fi
    sed 's/^/guest-log: /' /tmp/log | grep -iE "backend|config|interface|error|warn"
fi
poweroff -f
INIT
chmod +x rootfs/init
(cd rootfs && find . | cpio -o -H newc 2>/dev/null) > initrd.cpio

KVM=""
[ -w /dev/kvm ] && KVM="-enable-kvm"
echo "qemu ${KVM:-without KVM}"
timeout 900 qemu-system-x86_64 $KVM -m 512 -smp "${SMP:-2}" -nographic -no-reboot \
    -kernel "$VMLINUZ" -initrd initrd.cpio \
    -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
    -append "console=ttyS0 quiet panic=-1 rdinit=/init" > vm.log 2>&1 &
qemu=$!
# The server pings the guest's tunnel address once the client is up
PINGED=0
if [ -n "$CLIENT" ]; then
    i=0
    until grep -q "client-up" vm.log 2>/dev/null || [ $i -gt 600 ] ||
        ! kill -0 $qemu 2>/dev/null; do
        sleep 1; i=$((i + 1))
    done
    ping -c 3 -W 2 10.200.0.2 >/dev/null 2>&1 && PINGED=1
fi
wait $qemu || true
tr -d '\r' < vm.log | grep -E "^ok:|^FAIL:|^mode:|^concurrent|^read |^RESULT|^guest-log" \
    | tee result.txt
# The module's version is CAMEX_VERSION of version.h (#117), not git's
want=$(sed -n 's/^#define CAMEX_VERSION "\(.*\)"/\1/p' /tmp/m/version.h)
got=$(strings /tmp/m/camex.ko | sed -n 's/^version=//p' | head -1)
if [ -n "$want" ] && [ "$got" = "$want" ]; then
    echo "ok: camex.ko version $got (version.h)" | tee -a result.txt
else
    echo "FAIL: camex.ko version '$got', version.h '$want'" | tee -a result.txt
fi
if [ -n "$CLIENT" ]; then
    if [ $PINGED = 1 ]; then
        echo "ok: client: the server pings 10.200.0.2 through camex.ko" | tee -a result.txt
    else
        echo "FAIL: client: the server cannot ping 10.200.0.2" | tee -a result.txt
        tail -5 /tmp/server.log
    fi
    grep -q "^ok: client: no route on camex after SIGTERM" result.txt ||
        echo "FAIL: client: phase did not finish" | tee -a result.txt
fi
grep -q "^RESULT: PASS" result.txt && ! grep -q "^FAIL" result.txt || { tail -30 vm.log; exit 1; }
echo "VM TEST: PASS"
