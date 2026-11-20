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
make
```

## The full camex (optional features)

```sh
make CAMEX_L2=1 CAMEX_MESH=1
```

| Flag           | Adds                                                                                       |
|----------------|--------------------------------------------------------------------------------------------|
| `CAMEX_L2=1`   | TAP networks (`mode = tap`) and bridging a LAN (`--bridge`), Linux ([networks.md](networks.md#tap-networks-mode--tap)) |
| `CAMEX_MESH=1` | mesh networks (`mesh = yes`): peers direct through NATs, relay otherwise ([networks.md](networks.md#mesh-networks-mesh--yes)) |

Without them the build is the core camex, as before. The releases have both:
`camex-<arch>` and `camex-<arch>-full`.

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

| Platform | Toolchain                            | Size    |
|----------|--------------------------------------|---------|
| x86_64   | Bootlin x86-64 musl                  | ~195 KB |
| arm      | OpenIPC arm musleabi                 | ~200 KB |
| aarch64  | Bootlin aarch64 musl                 | ~205 KB |
| mipsel   | OpenWrt mipsel_24kc musl, soft-float | ~230 KB |

Release binaries are static and linked against musl: they need no libc on the
device and run on any OpenIPC firmware. They are not packed with UPX, so they
do not have to be unpacked into RAM on every start. Built by `ci/build-musl.sh`;
exact sizes are in the size report of each
[CI run](https://github.com/OpenIPC/camex/actions).
