<!-- Copyright (c) OpenIPC  https://openipc.org  MIT License -->

# Building

## Requirements

| Requirement  | Details                                                                                                                                  |
|--------------|------------------------------------------------------------------------------------------------------------------------------------------|
| Linux kernel | 3.10 or later                                                                                                                            |
| TUN support  | `CONFIG_TUN=y` (`/dev/net/tun`) **or** `camex.ko` loaded (`/dev/camex`)                                                                  |
| Privileges   | `root` or `CAP_NET_ADMIN` + `CAP_NET_RAW` (non-root with `camex.ko`: also access to `/dev/camex`, see [Kernel Module](kernel-module.md)) |
| C compiler   | C99-compatible (GCC or Clang)                                                                                                            |

## Userspace app

```sh
make                    # the full camex: everything (PROFILE=full, the default)
make PROFILE=client     # a client only: no server, with TAP networks and mesh
```

## Profiles and flags

A profile is a set of the flags below. A flag given on its own wins over its
profile, so an integration builds exactly what it needs:

```sh
make PROFILE=client CAMEX_L2=0 CAMEX_MESH=0   # the smallest client: TUN only
make CAMEX_MESH=0                             # a server without mesh
make CAMEX_WEB=0                              # a server without the start page
```

| Profile              | `CAMEX_SERVER` | `CAMEX_WEB` | `CAMEX_L2` | `CAMEX_MESH` | For                                                      |
|----------------------|----------------|-------------|------------|--------------|----------------------------------------------------------|
| `full` (the default) | 1              | 1           | 1          | 1            | servers, and anyone who wants everything: `camex-<arch>` |
| `client`             | 0              | 0           | 1          | 1            | cameras, routers, Android: `camex-<arch>-client`         |

`PROFILE=server` is the same as `full`. On Windows and macOS a profile leaves
`CAMEX_L2` out: TAP networks and `--bridge` are Linux only; on Windows also
`CAMEX_WEB`, which needs the control sockets.

| Flag             | Default (`full`) | What it adds                                                                                                                     |
|------------------|------------------|----------------------------------------------------------------------------------------------------------------------------------|
| `CAMEX_SERVER=1` | 1                | the server (`--mode server`), its control socket answers for `--status` and `--top`, `--format nginx-map` and `nginx-index`      |
| `CAMEX_WEB=1`    | 1                | the start page and nginx's lookup of the server (`web_listen=`, [nginx.md](nginx.md)), `--hash-password`; needs `CAMEX_SERVER=1` |
| `CAMEX_L2=1`     | 1 (Linux)        | TAP networks (`mode = tap`) and bridging a LAN (`--bridge`), Linux ([networks.md](networks.md#tap-networks-mode--tap))           |
| `CAMEX_MESH=1`   | 1                | mesh networks (`mesh = yes`): peers direct through NATs, relay otherwise ([networks.md](networks.md#mesh-networks-mesh--yes))    |
| `CAMEX_NEON=0`   | 1 (32-bit ARM)   | leaves out ChaCha20 and Poly1305 with NEON (below)                                                                               |

A camex built without a part refuses it and says why: without the server,
`--mode server` and `--format nginx-map` / `nginx-index` end with an error, and
`--help` shows no server options; without the web part, `web_listen=` is
ignored with a warning. `camex --version` names what the build has:

```
camex 5.2.0
features: client, server, web, l2, mesh
```

A build directory remembers the flags its objects were built with: another
profile or flag in the same `BUILD_DIR` rebuilds them all.

### A client for a firmware

A firmware (OpenIPC, OpenWrt, Keenetic) that runs camex as a client builds the
client profile with its own toolchain, and turns off what it does not use:

```sh
# OpenIPC (arm)
make CROSS_COMPILE=arm-openipc-linux-musleabi- LDLIBS=-static PROFILE=client
# OpenWrt (mipsel), with its SDK's toolchain
make CROSS_COMPILE=mipsel-openwrt-linux-musl- PROFILE=client CAMEX_L2=0
# Keenetic: Entware's toolchain (mipsel-3.4, aarch64-3.10), as an Entware package
# (glibc, which Entware puts on the router: not static, a static glibc binary
# cannot resolve the server's name)
make CROSS_COMPILE=mipsel-openwrt-linux-gnu- PROFILE=client
```

A static binary needs no libc on the device. Without a toolchain at hand,
`camex-arm-client` and `camex-mipsel-client` of the release are the client
profile, static (musl): `camex-mipsel-client` runs on Keenetic's MIPS models.

The Android library builds the client profile without L2 (VpnService has no
TAP) and, for now, without mesh (`android/camex-sources.cmake`).

## ChaCha20 and Poly1305 with NEON (32-bit ARM)

A build for 32-bit ARM also builds `chacha_neon.c`: ChaCha20 four blocks at a
time and Poly1305 two blocks at a time with NEON (Cortex-A7, A9 and the like);
on a hi3516ev300 (Cortex-A7) that seals 175 Mbit/s of 1400-byte packets instead
of 142 with ChaCha20 alone with NEON. It is used only when the kernel
says the CPU has NEON (`AT_HWCAP`), so one `camex-arm` still runs on ARMv5 and
ARMv6 cameras. `CAMEX_NEON=0` leaves it out. `camex --bench-crypto` prints how
fast the CPU seals 1400-byte tunnel packets, and whether NEON is used.

## Kernel module (optional)

```sh
# Prerequisites: kernel headers
make kmod
```

## Both

```sh
make && make kmod
```

## Cross-compile

```sh
make CROSS_COMPILE=mipsel-linux-
```

For the kernel module on an embedded kernel (OpenWrt and similar) see
[Building for an embedded kernel](kernel-module.md#building-for-an-embedded-kernel-openwrt-and-similar).

## Install kernel module

```sh
sudo make kmod-install
```

## Clean

```sh
make clean          # remove userspace build artifacts
make kmod-clean     # remove kernel module build artifacts
make distclean      # clean everything
```

## Binary sizes

Static ARM build (OpenIPC arm musleabi) of camex 5.2.0:

| Build                               | Size    |
|-------------------------------------|---------|
| `full` (`camex-arm`)                | ~375 KB |
| `full`, `CAMEX_WEB=0`               | ~360 KB |
| `full`, `CAMEX_L2=0 CAMEX_MESH=0`   | ~345 KB |
| `client` (`camex-arm-client`)       | ~270 KB |
| `client`, `CAMEX_L2=0 CAMEX_MESH=0` | ~250 KB |

Release binaries are static and linked against musl: they need no libc on the
device and run on any OpenIPC firmware. They are not packed with UPX, so they
do not have to be unpacked into RAM on every start. Built by `ci/build-musl.sh`;
exact sizes are in the size report of each
[CI run](https://github.com/OpenIPC/camex/actions).
