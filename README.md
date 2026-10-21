![OpenIPC logo][logo]

# OpenIPC Camex

**English** | [Русский](README.ru.md) | [简体中文](README.zh-CN.md)

[![camex CI](https://github.com/OpenIPC/camex/actions/workflows/build.yml/badge.svg)](https://github.com/OpenIPC/camex/actions/workflows/build.yml)
[![Release](https://img.shields.io/badge/download-latest-blue)](https://github.com/OpenIPC/camex/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Code size](https://img.shields.io/github/languages/code-size/OpenIPC/camex)](https://github.com/OpenIPC/camex)

Minimal dependency-free UDP/TCP tunnel for embedded Linux targets.

Ships as a userspace daemon and an optional standalone kernel module.

---

## Features

- **Client/server architecture** over UDP or TCP
- **Auto-config mode** — client fetches tunnel parameters from the server
- **Protocol v2** — Noise handshake (`NKpsk0`, X25519, ChaCha20-Poly1305,
  BLAKE2b), fresh keys per session and direction, always encrypted
- **No external dependencies** — only libc required
- **Cross-platform builds** — x86_64, ARM, AArch64, MIPS (via CI)
- **Custom kernel module** — `camex.ko` replaces `tun.ko` with no external deps
- **Compact codebase** — ~3,500 lines total, easy to audit

## Quick Start

### Keys

The server has a key pair; every client needs the server's **public** key and
a pre-shared key (the global one, or its own `psk=` from the server config).

```sh
(umask 077; camex --genkey > server.key)   # server_key=… (secret) and public_key=…
```

### Server

```sh
sudo ./camex --mode server --port 5800 --config /etc/camex/camex.conf \
  --server-key-file server.key --psk secret
```

### Client (auto mode)

```sh
sudo ./camex --mode client --auto --name 0203A104B5AE \
  --server-host vpn.example.org --port 5800 \
  --server-pubkey <public_key> --psk secret
```

### Client (manual mode)

```sh
sudo ./camex --mode client \
  --local-cidr 10.0.0.2/24 --gateway-ip 10.0.0.1 \
  --server-host vpn.example.org --port 5800 \
  --server-pubkey <public_key> --psk secret
```

### Android

The app connects in auto mode: the server sends the address, routes and MTU.

| Main screen                                  | Settings                                      |
|----------------------------------------------|-----------------------------------------------|
| ![Main screen](docs/images/android-main.png) | ![Settings](docs/images/android-settings.png) |

- **Server host / port** — the camex server.
- **Client name** — the client ID, i.e. the `[client ID]` block on the server.
- **Transport** — UDP or TCP, as on the server.
- **Server public key** and **Pre-shared key** — `--server-pubkey --psk`; the
  key must match the client's `psk=` (or the global key).
- **Route all traffic through the tunnel** — off: only the tunnel network and
  the server's routes go through the tunnel.
- **Advanced** — MTU, keepalive and server timeout
  ([screenshot](docs/images/android-settings-advanced.png)).

## Permanent links

The binaries are published as one release, [`latest`](https://github.com/OpenIPC/camex/releases/latest),
rebuilt by CI from every push to `master`. Its title shows the camex version
(`Camex X.Y.Z`); the file names carry no version, so these links never change:

- release page: `https://github.com/OpenIPC/camex/releases/latest`
- a file: `https://github.com/OpenIPC/camex/releases/latest/download/<file>`

| File                       | Platform                                                                      |
|----------------------------|-------------------------------------------------------------------------------|
| `camex-aarch64`            | Linux AArch64, static (musl)                                                  |
| `camex-arm`                | Linux 32-bit ARM, static (musl): OpenIPC cameras                              |
| `camex-mipsel`             | Linux MIPS little-endian, static (musl, soft-float): Ingenic cameras, routers |
| `camex-x86_64`             | Linux x86_64, static (musl)                                                   |
| `camex-android.apk`        | Android 8.0 and later                                                         |
| `camex-macos.zip`          | macOS (utun): `camex`, LICENSE, README                                        |
| `camex-windows-x86_64.zip` | Windows 64-bit: `camex.exe` and `wintun.dll`                                  |
| `camex-windows-i686.zip`   | Windows 32-bit: `camex.exe` and `wintun.dll`                                  |

```sh
URL=https://github.com/OpenIPC/camex/releases/latest/download
curl -fL -O $URL/camex-arm
```

The change history is in [CHANGELOG.md](CHANGELOG.md).

## Binary Sizes

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

## Requirements

| Requirement  | Details                                                                                                                                |
|--------------|----------------------------------------------------------------------------------------------------------------------------------------|
| Linux kernel | 3.10 or later                                                                                                                          |
| TUN support  | `CONFIG_TUN=y` (`/dev/net/tun`) **or** `camex.ko` loaded (`/dev/camex`)                                                                |
| Privileges   | `root` or `CAP_NET_ADMIN` + `CAP_NET_RAW` (non-root with `camex.ko`: also access to `/dev/camex`, see [Kernel Module](#kernel-module)) |
| C compiler   | C99-compatible (GCC or Clang)                                                                                                          |

## Build

### Userspace app

```sh
make
```

### Kernel module (optional)

```sh
# Prerequisites: kernel headers
make kmod
```

### Both

```sh
make && make kmod
```

### Cross-compile

```sh
make CROSS_COMPILE=mipsel-linux-
```

For the kernel module on an embedded kernel (OpenWrt and similar) see
[Building for an embedded kernel](#building-for-an-embedded-kernel-openwrt-and-similar).

### Install kernel module

```sh
sudo make kmod-install
```

### Clean

```sh
make clean          # remove userspace build artifacts
make kmod-clean     # remove kernel module build artifacts
make distclean      # clean everything
```

## TUN Driver Support

The app auto-detects the available TUN backend at startup. It tries `/dev/net/tun`
(tun.ko) first; if that fails or TUNSETIFF is unavailable, it falls back to
`/dev/camex` (camex.ko). Both backends work identically from the protocol perspective.

Override auto-detection with `--tun-dev <path>`:

```sh
sudo ./camex --mode client ... --tun-dev /dev/camex
```

| Feature                  | tun.ko (`/dev/net/tun`)           | camex.ko (`/dev/camex`) |
|--------------------------|-----------------------------------|-------------------------|
| Interface name           | dynamic (`tun0`, `tun1`, …)       | always `camex`          |
| Requires TUNSETIFF ioctl | Yes                               | No                      |
| External dependency      | `CONFIG_TUN=y` or `tun.ko` loaded | `camex.ko` loaded       |
| IPv4                     | Yes                               | Yes                     |
| IPv6                     | Yes                               | No (IPv4 only)          |
| Multiple instances       | Yes                               | No (one)                |
| Bare IPv4 packets        | Yes (with `IFF_NO_PI`)            | Yes                     |

## Userspace App

### Modes

| Mode     | Description                                          |
|----------|------------------------------------------------------|
| `client` | Creates a TUN interface and connects to the server   |
| `server` | Listens for many clients and forwards tunnel packets |

### Usage

#### Client

```sh
sudo ./camex \
  --mode client \
  --auto \
  --name 0203A104B5AE \
  --server-host vpn.example.org \
  --port 5800 \
  --server-pubkey <public_key> --psk secret
```

Manual client mode:

```sh
sudo ./camex \
  --mode client \
  --local-cidr 10.0.0.2/24 \
  --gateway-ip 10.0.0.1 \
  --server-host vpn.example.org \
  --port 5800 \
  --route-cidr 192.168.100.0/24 \
  --server-pubkey <public_key> --psk secret
```

With explicit TUN backend:

```sh
sudo ./camex \
  --mode client \
  --local-cidr 10.0.0.2/24 \
  --gateway-ip 10.0.0.1 \
  --server-host vpn.example.org \
  --port 5800 \
  --server-pubkey <public_key> --psk secret \
  --tun-dev /dev/camex
```

#### Server

```sh
sudo ./camex \
  --mode server \
  --port 5800 \
  --bind-ip 0.0.0.0 \
  --config /etc/camex/camex.conf \
  --server-key-file /etc/camex/server.key \
  --psk secret
```

### Options

| Option                     | Short | Description                                                                                                                |
|----------------------------|-------|----------------------------------------------------------------------------------------------------------------------------|
| `--mode <mode>`            | `-M`  | Operation mode: `client` or `server` (required)                                                                            |
| `--auto`                   | `-a`  | Fetch tunnel parameters from the server                                                                                    |
| `--name <id>`              | `-n`  | Client ID (auto mode; optional in manual mode)                                                                             |
| `--local-cidr <cidr>`      | `-l`  | Tunnel address in `address/prefix` form                                                                                    |
| `--gateway-ip <addr>`      | `-g`  | Client gateway inside the tunnel                                                                                           |
| `--server-host <addr>`     | `-s`  | Server hostname or IP for client mode                                                                                      |
| `--port <port>`            | `-p`  | Server port for client or listen port for server                                                                           |
| `--bind-ip <addr>`         | `-b`  | Server bind address (default: 0.0.0.0)                                                                                     |
| `--config <path>`          | `-f`  | Server config file path                                                                                                    |
| `--route-cidr <cidr>`      | `-c`  | Extra route to install on the client; repeatable                                                                           |
| `--mtu <size>`             | `-t`  | Tunnel MTU, 576–9000 (default: 1500)                                                                                       |
| `--psk <key>`              | `-k`  | Pre-shared key: `hex:<64 hex digits>`, `base64:<44 chars>`, or a passphrase (stretched with Argon2id). Required on clients |
| `--server-pubkey <key>`    |       | Client: the server's public key (from `--genkey`)                                                                          |
| `--server-key-file <path>` |       | Server: file with its private key (or `server_key=` in `[server]`)                                                         |
| `--genkey`                 |       | Print a new server key pair (`server_key=`, `public_key=`) and exit                                                        |
| `--encrypt`                | `-e`  | Accepted for compatibility; protocol v2 is always encrypted                                                                |
| `--tun-dev <path>`         | `-T`  | TUN device path (default: auto-detect `/dev/net/tun` then `/dev/camex`)                                                    |
| `--pid-file <path>`        | `-P`  | Write PID to file on startup                                                                                               |
| `--bind-dev <iface>`       | `-d`  | Bind socket to a specific network interface (`SO_BINDTODEVICE`)                                                            |
| `--transport <proto>`      | `-R`  | Transport protocol: `udp` (default) or `tcp`                                                                               |
| `--version`                | `-v`  | Print version and exit                                                                                                     |
| `--help`                   | `-h`  | Print usage and exit                                                                                                       |

## Examples

Ready-to-use configuration files are available in the `examples/` directory:

| File                     | Description                                     |
|--------------------------|-------------------------------------------------|
| `examples/camex.conf`    | Server-side configuration file format reference |
| `examples/camex.service` | systemd service unit for camex daemon           |
| `examples/README.md`     | Usage instructions for all examples             |

## Server Configuration

The server reads a flat key=value config file with one client record per block.
See `examples/camex.conf` for the full format.

```sh
sudo ./camex --mode server --port 5800 --config /etc/camex/camex.conf
```

Auto-mode clients get their addresses from the config file. A client whose ID
has no `[client ID]` block gets `[defaults]`, or, when no address is configured,
one from the auto pool, `10.200.0.2`–`10.200.0.254` by default (addresses in use
and those of `[client ID]` blocks are skipped; addresses of expired clients are
reused). To serve only the IDs listed in the file, set `allow_unknown_clients=no`
in the `[server]` block.

After a change to the file, `kill -HUP` the server. A running auto-mode client
picks up a changed address, gateway, MTU or routes with its next handshake (a
key renewal, at most 10 minutes later, or a reconnect): it reopens its tunnel
with the new settings and replaces its routes, without a restart.

### Auto pool and number of clients

Two `[server]` keys size the server for its network:

- `auto_pool=<network>/<prefix>` — the auto pool, from `/8` to `/30`; default
  `10.200.0.0/24`. The first address is the gateway the clients get, so give it
  to the server with `--local-cidr` or `local_cidr=` (for the default pool
  `10.200.0.1/24`). Clients get the second to the last but one address, with
  the pool's prefix (`10.50.3.7/16` from `10.50.0.0/16`). The server warns at
  startup and after a reload when its tunnel address is not the pool's gateway
  or the pool is outside its tunnel network. After a reload with a new pool,
  auto-mode clients move to it with their next handshake.
- `max_clients=<n>` — client slots, from 1 to 65534; default: the number of
  pool addresses, at least 256 (a `/24` or smaller pool keeps 256). Read at
  startup only. Over TCP the server uses `select()` and keeps at most
  `FD_SETSIZE` slots (1024 on Linux); the log says so.

```ini
[server]
local_cidr=10.50.0.1/16
# 65533 client addresses and as many slots
auto_pool=10.50.0.0/16
```

A slot takes about 1.3 KB and a `[client ID]` block about 0.7 KB: 256 slots
about 330 KB, 65534 about 85 MB. Finding a client does not depend on their
number. Large networks are meant for a PC or a server; on a router keep the
default `/24`. The server locks its memory (`mlockall`) so that keys are not
swapped out; give it a memory lock limit for its slots (root has none;
otherwise `ulimit -l`, `LimitMEMLOCK=infinity` in systemd, `--cap-add IPC_LOCK`
or `--ulimit memlock=-1` in Docker), or it runs unlocked with a `SECURITY
WARNING` in the log. With thousands of `[client ID]` blocks, give their `psk=`
as `hex:` or `base64:` keys: each passphrase costs an Argon2id run (8 MiB, tens
of milliseconds) when the server starts, and on reloads beyond the 260 keys it
caches.

The session index in each packet carries the client's slot number in its low
16 bits (the high 16 bits are random), so the server finds a packet's session
without a search.

## Security Model

camex is a small tunnel for cameras, not a general-purpose VPN. Know what it
protects before exposing it to an untrusted network.

### Protocol v2 (camex 3.x)

- **Handshake.** `Noise_NKpsk0_25519_ChaChaPoly_BLAKE2b`: the client knows the
  server's public key, both sides know the client's pre-shared key. Every
  session gets fresh keys, separate for each direction (forward secrecy: a
  leaked key does not decrypt recorded sessions).
- **Packets.** ChaCha20-Poly1305: the payload is encrypted, the 17-byte header
  (type, session index, counter) is authenticated but readable. Packets that
  fail authentication are dropped. Keys are renewed every 10 minutes.
- **Keys.** The pre-shared key is a raw key (`hex:`, `base64:`) or a passphrase
  stretched with Argon2id. A captured handshake does not let anyone test
  passphrase guesses offline: that needs the server's private key.
- **Per-client keys.** A `psk=` in a `[client ID]` block gives that client its
  own key; it can register only with it, and the global key no longer works for
  that ID (unless both passphrases are the same, which makes them one key). See
  [Notes](#notes) for the exact rules.
- **Source addresses.** A client may send only from its own tunnel address;
  packets with another source address are dropped.
- **Replay protection.** Data packets have a 2048-packet window per session.
  While a client has a live session, a handshake with an older timestamp is
  rejected; a replayed handshake can never take a working session over, since
  a new session replaces the old one only with the client's first packet on it
  (a client whose clock went back after a reboot still reconnects). Packets of
  an ended session cannot be replayed into a new one: its keys are different.
- **Roaming.** A session is found by its index, not by the client's address:
  after a NAT rebinding the client keeps its session.
- **Config reload.** After `SIGHUP`, a live session whose key was removed or
  changed, or now belongs to another client, ends.

### What it does not protect

- **The server's private key** lets its holder impersonate the server to
  clients that know the pre-shared key: keep `server_key` secret.
- **Metadata.** Packet sizes, timing, endpoints and the cleartext header (packet
  type, session index, counter) are visible.
- There is **no unencrypted mode**; for an open tunnel without encryption see
  OpenIPC VTesse.
- **Inside the tunnel** traffic is not filtered: any client can reach the server
  and other clients on the tunnel network. Restrict that with a firewall on the
  server's tunnel interface (`tun0` or similar with `/dev/net/tun`, `camex`
  with the kernel module; the server logs its name at startup).

### Denial of service limits

- TCP connections that have not completed a handshake are limited (4 per
  source address, 32 in total) and closed after 10 s. A handshake carries a
  MAC keyed with the server's public key: packets from anyone who does not know
  it are dropped after one hash. The server processes at most 200 handshakes
  per second.
- Log messages that a peer can trigger (dropped packets, rejected
  registrations) are rate-limited; the number suppressed is logged.
- The auto pool hands out only client addresses of its network (by default
  `10.200.0.2`–`.254`) and never gives one address to two live clients (an expired client's address is handed out again; a client that
  reconnects, also after a server restart, gets its previous address back while
  it is free);
  `allow_unknown_clients=no` serves only auto-mode IDs listed in the config file
  (manual clients are bound by their key and address, see [Notes](#notes)).

### Recommended deployment

- Keep the server key file readable by the server only (`umask 077`); give the
  clients `public_key` only.
- Prefer raw keys (`hex:`/`base64:`) or long random passphrases.
- Give each camera its own `psk=` in a `[client ID]` block, and set
  `allow_unknown_clients=no` when all auto-mode cameras are listed.
- Run the server with only the capabilities it needs (`CAP_NET_ADMIN`; with the
  kernel module also file access to `/dev/camex`, see below), and firewall the
  tunnel interface.

### Kernel module permissions

`/dev/camex` is created `0600`, and `open()` requires `CAP_NET_ADMIN`; see
[Key Design Decisions](#key-design-decisions). The module does no encryption:
encryption is done by the userspace app.

### Android app

The app uses `VpnService`, so Android shows the VPN key icon while the tunnel is
up; there is no way around that with `VpnService` (a client without TUN is
tracked in #21). Only the tunnel network and the routes the server sends go
through the tunnel; a `0.0.0.0/0` route from the server is ignored unless
**Route all traffic through the tunnel** is enabled in the settings. That
switch covers IPv4 only: IPv6 traffic does not go through the tunnel.

## Kernel Module

### Architecture

```
Userspace                          Kernel
──────────────────                 ──────────────────────────────────────────
                                   ┌─────────────────────────────────┐
  write(fd, ipv4_pkt)  ──────────► │  copy_from_user()               │
                                   │  netif_rx(skb)                  │
                                   │       │                         │
                                   │       ▼                         │
                                   │  IP stack / routing             │
                                   │       │                         │
                                   │       ▼                         │
                                   │  camex_net_xmit()               │
                                   │  enqueue → rx_queue             │
                                   │  wake_up(rx_wait)               │
  read(fd, buf)        ◄────────── │  copy_to_user()                 │
                                   └─────────────────────────────────┘
  poll/select/epoll    ◄────────── wait_queue (rx_wait)

  /dev/camex  ←────────────────────────────────────── net interface "camex"
```

Two independent kernel objects — a character device (`miscdevice`) and
a network interface (`net_device`) — share a single `camex_priv`
instance. Communication between them flows through the `rx_queue`
protected by a `spinlock`.

### Key Design Decisions

#### 1. No dependency on `tun.ko`

The upstream `/dev/net/tun` is a separate module (`drivers/net/tun.c`)
that must be either loaded or compiled into the kernel. camex registers
its own `net_device` and `miscdevice` directly, using only core kernel
subsystems (`netdev`, `misc`, `skbuff`) that are **always present** and
cannot be unloaded or disabled.

#### 2. `miscdevice` instead of manual `chrdev` registration

Using `misc_register()` automatically:
- allocates a minor number dynamically (no conflicts);
- creates `/dev/camex` via udev/devtmpfs without any udev rules;
- sets permissions to `0600` (root only).

`open()` additionally requires `CAP_NET_ADMIN`: the device injects
packets into the network stack and exposes routed traffic, which is a
network-admin operation, just like configuring a tun device.

A daemon that runs as a non-root user with `CAP_NET_ADMIN` + `CAP_NET_RAW`
also needs file access to the node, which `0600` does not give it. Grant it
to a dedicated group with a udev rule (the capability check still applies):

```sh
# /etc/udev/rules.d/90-camex.rules
KERNEL=="camex", GROUP="camex", MODE="0660"
```

or, without udev, after each `insmod`: `chgrp camex /dev/camex && chmod 0660 /dev/camex`.

#### 3. Packet queue protected by `spinlock` + `irqsave`

`camex_net_xmit()` is called from softirq context (not from process
context), so `spin_lock_irqsave` is used instead of a mutex to protect
the queue. This prevents deadlocks when called from interrupt context.

#### 4. Blocking `read()` via `wait_event_interruptible`

Instead of busy-waiting or timers, the standard kernel mechanism is
used: the process sleeps on a `wait_queue`, and `camex_net_xmit()`
wakes it up via `wake_up_interruptible()`. Signals correctly interrupt
the wait (`-ERESTARTSYS`).

#### 5. IPv4-only with explicit validation on `write()`

On `write()`, the first byte of the buffer is checked before
`copy_from_user` via `get_user()`. If the IP version is not 4,
`-EINVAL` is returned without allocating an `skb`, preventing garbage
or non-IPv4 packets from entering the stack.

#### 6. Single user (`EBUSY`)

`g_priv->attached` is an `atomic_t` taken with `atomic_cmpxchg()`. If
`/dev/camex` is already open, a subsequent `open()` immediately returns
`-EBUSY`, and two concurrent `open()` calls can never both succeed. On
`close()` the queue is flushed and the slot released under the same lock
that `camex_net_xmit()` takes, so no packet is left for the next `open()`.

#### 7. `ARPHRD_NONE` + `IFF_NOARP`

The interface is declared as having no link-layer address. The kernel
does not attempt to resolve MAC addresses (ARP) and does not prepend
Ethernet headers. Userspace sends and receives **bare IPv4 datagrams**
starting at the version byte `0x45...`.

#### 8. Compatibility via `#define` shims

All `#if LINUX_VERSION_CODE` guards are grouped into a single `#define`
block at the top of the file. The driver body uses only macro names
(`CAMEX_ALLOC_NETDEV`, `CAMEX_POLL_T`, `CAMEX_SKB_PUT`, etc.) with no
conditional compilation inside functions. This simplifies auditing and
maintenance.

### Kernel Compatibility

| Range         | Status | Adapted API                                             |
|---------------|--------|---------------------------------------------------------|
| 3.10 – 3.16   | ✅     | `alloc_netdev(sz, name, setup)` — 3 arguments           |
| 3.17 – 3.19   | ✅     | `alloc_netdev` + `NET_NAME_PREDICTABLE` — 4 arguments   |
| 4.0  – 4.9    | ✅     | `min_mtu`/`max_mtu` fields absent from `net_device`     |
| 4.10 – 4.15   | ✅     | `min_mtu`/`max_mtu` fields introduced                   |
| 4.16 – 4.19   | ✅     | `poll` returns `__poll_t`; `EPOLL*` constants           |
| 4.20+         | ✅     | `skb_put()` returns `void*` instead of `unsigned char*` |
| 5.x           | ✅     | no changes required                                     |
| 6.0 – 6.6 LTS | ✅     | no changes required                                     |

> **Note**: Kernels 3.0 – 3.9 are theoretically covered by the same
> shims but are extremely rare in practice (RHEL 6, early embedded).

### Building and Installation

#### Prerequisites

```bash
# Debian / Ubuntu
sudo apt install linux-headers-$(uname -r) build-essential

# RHEL / CentOS 7
sudo yum install kernel-devel-$(uname -r) gcc make

# Fedora / RHEL 8+
sudo dnf install kernel-devel gcc make

# Alpine Linux
sudo apk add linux-headers build-base
```

#### Compile

```bash
make kmod
```

#### Load the module

```bash
sudo insmod camex.ko
```

Verify:

```bash
dmesg | tail -5
# camex: loaded — /dev/camex <-> net:camex  (kernel 5.15)

ls -la /dev/camex
# crw------- 1 root root 10, 58 ...  /dev/camex

ip link show camex
# camex: <POINTOPOINT,NOARP> mtu 1500 ...
```

#### Persistent load at boot

```bash
# Install the module into the kernel tree
sudo make kmod-install     # copies camex.ko and runs depmod -a

# Enable autoload
echo "camex" | sudo tee /etc/modules-load.d/camex.conf
```

#### Unload

```bash
sudo rmmod camex
```

> Unloading is blocked while `/dev/camex` is held open by any process
> (`rmmod` returns `EBUSY`).

#### Building for an embedded kernel (OpenWrt and similar)

Firmware kernels rarely come with a headers package, and OpenWrt strips
`vermagic` from its modules and builds without `CONFIG_MODVERSIONS`: `insmod`
loads any `camex.ko`, even one built for another kernel configuration. Such a
module loads and the tunnel comes up, but its offsets into kernel structures
(`struct sk_buff`, `struct net_device`) are wrong: every packet is dropped, and
the module can corrupt kernel memory. Build it against the kernel that runs on
the device:

1. **Same kernel source and compiler.** The same kernel version with the
   firmware's patches (OpenWrt: the release branch at the commit that set this
   kernel version, `target/linux/generic` and `target/linux/<target>` —
   `files/` and `patches-*`), built with the same compiler (the OpenWrt SDK
   toolchain of that release and target).
2. **Same kernel config.** If the device has `/proc/config.gz`, use it. If not,
   start from the SDK or build-system config and turn off what the device
   lacks: compare its `/proc/kallsyms` (functions only, unless
   `CONFIG_KALLSYMS_ALL`) and `/proc/net`. Options that change `struct sock`,
   `struct sk_buff` or `struct net_device` matter most, for example `IPV6`,
   `XFRM`, `BRIDGE_NETFILTER`, `NET_CLS_ACT`, `NETWORK_SECMARK`,
   `NET_RX_BUSY_POLL`, `NF_CONNTRACK`. An option turned on by `select` goes
   off only together with the options that select it.
3. **Prepare the tree and build:**

   ```sh
   make -C <kernel-tree> ARCH=mips CROSS_COMPILE=<prefix> olddefconfig modules_prepare
   make kmod KDIR=<kernel-tree> ARCH=mips CROSS_COMPILE=<prefix>
   ```

   If the host tools of an older kernel fail to link with GCC 10 or newer
   (`dtc`: multiple definition of `yylloc`), add `HOSTCFLAGS=-fcommon`.
4. **Check the layout before loading.** Build a module the device already has
   from the same tree, for example
   `make -C <kernel-tree> ARCH=mips CROSS_COMPILE=<prefix> drivers/net/tun.ko`,
   and compare it with the device's `/lib/modules/$(uname -r)/tun.ko`: the
   `.text` size (`objdump -h`) and the offsets in every load and store
   (`objdump -d`, e.g. `lw v0,452(a0)`) must be the same. Instruction order may
   differ with another build of the same compiler; offsets must not.
5. **Symptom of a mismatch.** The handshake and auto-config work, but there is
   no traffic, and `rx_dropped` of `camex` (`cat /proc/net/dev`) grows with
   every packet from the server. Stop the client and `rmmod camex` at once.

Tested example: OpenWrt 15.05.1 ramips/mt7620 (MT7620N, MIPS 24KEc) with a
custom kernel 3.18.92 — linux 3.18.92 with the patches of `openwrt/chaos_calmer`
at `31cce51cc7` ("kernel: bump to 3.18.92"), the toolchain and `.config` of the
OpenWrt SDK 15.05.1 ramips/mt7620 with `IPV6`, `XFRM` (and `INET_AH`,
`INET_ESP`, `INET_IPCOMP`, `INET_XFRM_MODE_*`, `NET_KEY`, `XFRM_USER`,
`XFRM_IPCOMP`, `XFRM_ALGO`, which select it) and `BRIDGE_NETFILTER` turned off.
With the unchanged SDK config the module loaded but dropped every packet
([#73](https://github.com/OpenIPC/camex/issues/73)).

### Character Device API

| Operation               | Behaviour                                                  |
|-------------------------|------------------------------------------------------------|
| `open()`                | Needs `CAP_NET_ADMIN`; one fd at a time, `EBUSY` otherwise |
| `read()`                | Returns one IPv4 packet. Blocks when queue is empty        |
| `read()` + `O_NONBLOCK` | Returns `EAGAIN` when queue is empty                       |
| `write()`               | Injects one IPv4 packet into the stack. Minimum 20 bytes   |
| `write()` non-IPv4      | Returns `EINVAL`                                           |
| `poll()`                | `POLLIN` when a packet is available; `POLLOUT` always      |
| `close()`               | Flushes the queue, clears carrier, releases the slot       |

#### Error codes

| Code       | Cause                                                       |
|------------|-------------------------------------------------------------|
| `EPERM`    | `open()` without `CAP_NET_ADMIN`                            |
| `EBUSY`    | `/dev/camex` is already open by another process             |
| `ENODEV`   | Module is being unloaded                                    |
| `EMSGSIZE` | `read()` buffer smaller than packet length (packet dropped) |
| `EINVAL`   | `write()`: size < 20 or > 65535 bytes, or not IPv4          |
| `EAGAIN`   | `read()` with `O_NONBLOCK`, queue is empty                  |
| `ENOMEM`   | Failed to allocate an `skb`                                 |

### Limitations

#### Functional

- **IPv4 only.** IPv6 is intentionally excluded. A `write()` with an
  IPv6 packet returns `-EINVAL`.

- **Single process at a time.** Only one file descriptor may hold
  `/dev/camex` open. A second `open()` returns `EBUSY`. For
  multi-process schemes, multiplex via a Unix socket in userspace.

- **Single device instance.** Exactly one `camex` interface and one
  `/dev/camex` device are created. Supporting multiple instances
  requires extending the code with a `g_priv[]` array and name suffixes.

- **TUN (Layer 3) only.** TAP (Layer 2, Ethernet frames) is not
  implemented. The interface has no MAC address (`ARPHRD_NONE`).
  ARP does not work.

- **No ioctl compatibility with `/dev/net/tun`.** The commands
  `TUNSETIFF`, `TUNGETIFF`, `TUNSETPERSIST`, and other ioctls from the
  upstream driver are not implemented. Programs that use
  `ioctl(fd, TUNSETIFF, ...)` will not work with camex without
  modification.

- **MTU is fixed at 1500 bytes.** Changing it via
  `ip link set camex mtu N` is not blocked by the kernel, but camex
  does not enforce the MTU on `write()`.

#### Queue and Memory

- **Queue depth is 64 packets.** When full, new packets are silently
  dropped and `tx_dropped` is incremented (visible in `ip -s link`).
  Adjust the `QUEUE_LIMIT` constant in the source.

- **Each queued packet is stored as an `sk_buff`.** Every `sk_buff`
  consumes ~200 bytes of header plus packet data. At 64 × 1500 bytes
  the maximum queue memory footprint is approximately 96–110 KB.

- **A `read()` with a too-small buffer destroys the packet.** If the
  buffer is smaller than the packet length, the packet is lost and
  `EMSGSIZE` is returned. Always read with a buffer of at least
  1500 bytes (or the interface MTU).

#### Thread Safety

- Concurrent `read()` calls from multiple threads of the same process
  are not serialised at the fd level — protect access with a mutex in
  userspace.

- Concurrent `write()` calls from multiple threads are safe: each call
  independently allocates its own `skb`.

#### Networking

- **No masquerading/NAT out of the box.** To route traffic from
  `camex` to the internet, configure `iptables -t nat -A POSTROUTING`
  manually.

- **`netif_rx()` vs `netif_receive_skb()`.** The driver uses
  `netif_rx()` (the older softirq-deferred path). On high-throughput
  systems, `netif_receive_skb()` offers lower latency but requires
  process or softirq context — the substitution is trivial if needed.

### Comparison with `tun.ko`

| Feature                        | camex            | tun.ko          |
|--------------------------------|------------------|-----------------|
| External module dependency     | No               | Requires tun.ko |
| IPv4                           | Yes              | Yes             |
| IPv6                           | No (intentional) | Yes             |
| TAP (L2)                       | No               | Yes             |
| Multiple device instances      | No (one)         | Yes (dynamic)   |
| Multiple open file descriptors | No (one)         | Yes             |
| ioctl TUNSETIFF / TUNGETIFF    | No               | Yes             |
| Persistent device              | No               | Yes             |
| poll / select / epoll          | Yes              | Yes             |
| O_NONBLOCK                     | Yes              | Yes             |
| Statistics (`ip -s link`)      | Yes              | Yes             |
| Kernel 3.x support             | Yes              | Yes             |
| Code size                      | ~490 lines       | ~3500 lines     |

### Internal Specifics

#### Compatibility Shims

All `#if LINUX_VERSION_CODE` guards are placed in a dedicated `#define`
block at the top of `camex-k.c`. The driver body uses only macro names
(`CAMEX_ALLOC_NETDEV`, `CAMEX_POLL_T`, `CAMEX_SKB_PUT`, etc.) with no
version conditionals inside functions. This keeps the logic readable and
easy to audit.

#### Registration and Deregistration Order

On `insmod`:
1. `alloc_netdev` + `camex_priv` initialisation
2. `register_netdev` → interface appears in `ip link`
3. `misc_register` → `/dev/camex` appears in the filesystem

On `rmmod`:
1. `misc_deregister` → `/dev/camex` disappears (new `open()` calls blocked)
2. `g_priv = NULL` → protects against use-after-free in xmit path
3. `unregister_netdev` + `free_netdev`

The reverse order guarantees that by the time memory is freed, no new
accesses can arrive through either path (fd or netdev).

#### Carrier and Link State

- `netif_carrier_off` on init — the interface shows `NO-CARRIER` until
  userspace opens `/dev/camex`.
- `netif_carrier_on` on `open()` — carrier becomes active.
- `netif_carrier_off` on `close()` — carrier is cleared; routes through
  the interface become unreachable.

#### `GFP_ATOMIC` in `xmit`

`camex_net_xmit()` is called from softirq context where sleeping is
forbidden. Therefore `kmalloc` for `pkt_node` uses `GFP_ATOMIC`. If
memory is unavailable in this context the packet is dropped
(`tx_dropped++`).

## Versioning & Release Process

The change history of every version is in [CHANGELOG.md](CHANGELOG.md); a version
bump adds its section there in the same PR.

### Single source of truth

The **only** place to change the software version is `src/version.h`:

```c
#define CAMEX_VERSION "X.Y.Z"
```

The Android `versionName` and `versionCode` follow it: `scripts/sync-android-version.sh`
(or `make apk`, which calls it) updates `android/app/build.gradle.kts` and
`android/app/src/main/AndroidManifest.xml`.

### When to bump

`CAMEX_VERSION` is `MAJOR.MINOR.PATCH`, numbers without leading zeros:

- **PATCH** — fixes and small changes; **MINOR** — a new feature (option, config field,
  protocol, platform); **MAJOR** — an incompatible change (arguments, config format,
  protocol, default behaviour), only when the maintainer decides.
- A pull request that changes the code or the build (`src/`, `android/`, `Makefile`,
  `ci/`, `scripts/`) bumps the version as its last commit before the merge and adds
  its `## X.Y.Z — YYYY-MM-DD` section to `CHANGELOG.md`. Pull requests with only docs,
  tests, screenshots or workflows in `.github/` do not.
- CI checks this on every pull request (`.github/check-version.sh`, job "Version
  bump"): the new version must be well-formed, greater than the one on `master` and
  have its new `CHANGELOG.md` section. Until the bump commit is pushed, the check is
  red by design.

### Release process

There are no version tags and no versioned releases: one release, `latest`, is
replaced by CI after every push to `master`, once all build and test jobs pass.

1. **In a PR**: bump `CAMEX_VERSION`, run `scripts/sync-android-version.sh`, add the
   version's section to `CHANGELOG.md`.
2. **Merge into `master`**. CI deletes the old `latest` release and its tag and creates
   it again on the built commit: title `Camex X.Y.Z`, the files from
   [Permanent links](#permanent-links).
3. **Telegram**: when the version differs from the title of the previous `latest`, CI
   posts the APK to the OpenIPC Telegram channel. A push without a version bump only
   replaces the files.

### Version numbering rules

- `versionName` (Android) = `CAMEX_VERSION` (e.g., `"3.0.2"`)
- `versionCode` (Android) = `MAJOR * 10000 + MINOR * 100 + PATCH` (e.g., `30002`)
- GitHub Release title = `Camex X.Y.Z`, tag `latest`

### Manual CI trigger

To rebuild and republish `latest` without a new commit, run the workflow with
`workflow_dispatch` on `master` in the Actions tab.

## Notes

- Each client should use a unique tunnel IP.
- The server relays packets based on the inner IPv4 destination address.
- Client sockets are outbound only; no listening port is opened on the client side.
- In auto mode, `-l`, `-g`, and `-c` are supplied by the server.
- `examples/camex.conf` shows the server-side parameter database format.
- Protocol v2 works over both UDP and TCP transport; every session starts with
  a handshake (`--server-key-file` on the server, `--server-pubkey` and `--psk`
  on the client). camex 3.x does not talk to camex 2.x.
- The server supports per-client PSKs: add `psk=<key>` inside a `[client ID]` block
  in the config file. Keys are bound to client IDs:
  - a client that has its own key can register only with that key;
  - the global `--psk` key works only for clients without their own key;
  - a manual client with its own key may omit `--name`: the key identifies it;
  - a manual client may only claim the `local_cidr` address of its own
    `[client ID]` block, and never the address of another client that has its
    own key.
- The server accepts data packets only from an established session and only with
  the client's own tunnel address as the source; other source addresses are
  dropped.

## License

[MIT](LICENSE). See [LICENSE](LICENSE) for the full terms.

## Support

- Questions and bug reports: [GitHub Issues](https://github.com/OpenIPC/camex/issues)
- Documentation: [openipc.org](https://openipc.org)

[![Telegram](https://openipc.org/images/telegram_button.svg)](https://openipc.org/our-channels)

[logo]: https://raw.githubusercontent.com/OpenIPC/.github/refs/heads/main/assets/openipc-logo-black.svg
