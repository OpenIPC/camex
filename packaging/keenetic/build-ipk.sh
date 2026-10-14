#!/bin/sh
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# build-ipk.sh — pack a static camex binary into an Entware .ipk (issue #33)
#
# The .ipk is a gzipped tar of debian-binary, control.tar.gz and data.tar.gz,
# the format ipkg-build produces for OpenWrt and Entware.
#
# Usage: build-ipk.sh <binary> <entware-arch> <version> <outdir>
#   entware-arch: mipsel-3.4 | mips-3.4 | aarch64-3.10
#
# Package layout:
#   /opt/sbin/camex                           static binary
#   /opt/etc/init.d/S99camex                  Entware rc.func init script
#   /opt/etc/camex/camex.env                  init script settings (conffile)
#   /opt/etc/camex/camex.conf                 sample server config (conffile)
#   /opt/etc/ndm/netfilter.d/50-camex.sh      KeeneticOS netfilter hook (draft)
#

set -eu

if [ $# -ne 4 ]; then
    echo "usage: $0 <binary> <entware-arch> <version> <outdir>" >&2
    exit 1
fi

BIN="$1"
ARCH="$2"
# "2.3.8-9-gab8075b" (git describe) -> "2.3.8.9.gab8075b": opkg splits on "-"
VERSION="$(echo "${3#v}" | tr "-" ".")"
mkdir -p "$4"
OUTDIR="$(cd "$4" && pwd)"

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# opkg wants a version that starts with a digit
case "$VERSION" in
    [0-9]*) ;;
    *) VERSION="0.0.0-$VERSION" ;;
esac

mkdir -p "$WORK/data/opt/sbin" "$WORK/control"
cp -R "$HERE/files/opt/." "$WORK/data/opt/"
cp "$REPO/examples/camex.conf" "$WORK/data/opt/etc/camex/camex.conf"
install -m 0755 "$BIN" "$WORK/data/opt/sbin/camex"
chmod 0755 "$WORK/data/opt/etc/init.d/S99camex" \
           "$WORK/data/opt/etc/ndm/netfilter.d/50-camex.sh"
chmod 0600 "$WORK/data/opt/etc/camex/camex.env"
chmod 0644 "$WORK/data/opt/etc/camex/camex.conf"

SIZE=$(du -sk "$WORK/data" | cut -f1)

cat > "$WORK/control/control" <<EOF
Package: camex
Version: $VERSION
Architecture: $ARCH
Maintainer: OpenIPC <dev@openipc.org>
Section: net
Priority: optional
Installed-Size: $SIZE
Source: https://github.com/OpenIPC/camex
License: MIT
Description: Minimal dependency-free UDP/TCP tunnel (OpenIPC camex).
 Static build for Keenetic routers running Entware. Configure
 /opt/etc/camex/camex.env, then start /opt/etc/init.d/S99camex.
EOF

cat > "$WORK/control/conffiles" <<'EOF'
/opt/etc/camex/camex.env
/opt/etc/camex/camex.conf
EOF

cat > "$WORK/control/prerm" <<'EOF'
#!/bin/sh
/opt/etc/init.d/S99camex stop >/dev/null 2>&1
exit 0
EOF
chmod 0755 "$WORK/control/prerm"

echo "2.0" > "$WORK/debian-binary"

# Root-owned archives, no group/other write bits
TAR_OPTS="--format=gnu --owner=0 --group=0 --numeric-owner --mode=go-w"
# shellcheck disable=SC2086
tar $TAR_OPTS -C "$WORK/control" -czf "$WORK/control.tar.gz" .
# shellcheck disable=SC2086
tar $TAR_OPTS -C "$WORK/data" -czf "$WORK/data.tar.gz" .

IPK="$OUTDIR/camex_${VERSION}_${ARCH}.ipk"
rm -f "$IPK"
# Outer container as made by OpenWrt/Entware ipkg-build: a gzipped tar
# (current opkg rejects the old ar format), members in this order.
# shellcheck disable=SC2086
tar $TAR_OPTS -C "$WORK" -czf "$IPK" ./debian-binary ./control.tar.gz ./data.tar.gz
echo "$IPK"
