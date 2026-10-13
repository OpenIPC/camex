#!/bin/sh
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# run-vm.sh — runs inside a Debian/Ubuntu container (see
# docker/test-kmod-vm.sh): builds camex.ko against the distro kernel
# headers, boots the matching kernel in QEMU with a busybox initramfs and
# runs kmodtest in the guest. Expects the sources in /k/src and /k/kmod.
#
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq linux-headers-generic qemu-system-x86 busybox-static \
    gcc make cpio >/dev/null 2>&1
K=$(ls -d /usr/src/linux-headers-*-generic | head -1)
KVER=${K#/usr/src/linux-headers-}
echo "kernel: $KVER"
cd /tmp
apt-get download -qq "linux-image-unsigned-$KVER" >/dev/null 2>&1 || \
    apt-get download -qq "linux-image-$KVER" >/dev/null 2>&1
dpkg -x linux-image-*"$KVER"*.deb img
VMLINUZ=$(ls img/boot/vmlinuz-* | head -1)

gcc -static -O1 -pthread -o kmodtest /k/kmod/kmodtest.c
cp -r /k/src /tmp/m
make -s -C "$K" M=/tmp/m modules 2>&1 | grep -v "compiler differs\|built by\|You are using\|BTF" || true

mkdir -p rootfs/bin rootfs/dev rootfs/proc rootfs/sys
cp /bin/busybox rootfs/bin/busybox
for a in sh mount insmod ip chmod sleep poweroff; do ln -s busybox rootfs/bin/$a; done
cp /tmp/m/camex.ko kmodtest rootfs/
cat > rootfs/init <<'INIT'
#!/bin/sh
mount -t devtmpfs dev /dev
mount -t proc proc /proc
mount -t sysfs sys /sys
insmod /camex.ko
sleep 1
/kmodtest
poweroff -f
INIT
chmod +x rootfs/init
(cd rootfs && find . | cpio -o -H newc 2>/dev/null) > initrd.cpio

KVM=""
[ -w /dev/kvm ] && KVM="-enable-kvm"
timeout 600 qemu-system-x86_64 $KVM -m 512 -smp "${SMP:-2}" -nographic -no-reboot \
    -kernel "$VMLINUZ" -initrd initrd.cpio \
    -append "console=ttyS0 quiet panic=-1 rdinit=/init" 2>&1 \
    | grep -E "^ok:|^FAIL:|^mode:|^concurrent|^read |^RESULT" | tee result.txt
grep -q "^RESULT: PASS" result.txt
