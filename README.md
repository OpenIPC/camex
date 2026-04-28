# camex

Minimal dependency-free Linux UDP/TUN tunnel for embedded targets.
Ships as a userspace daemon **camexd** and an optional standalone kernel module **camex_kmod**.

---

## Overview

```
Host A (camex0)                          Host B (camex0)
  10.0.0.1 ──► camexd/kmod ──UDP──► camexd/kmod ──► 10.0.0.2
```

* Raw IP packets read from a TUN interface are encapsulated in UDP and forwarded to the configured peer, and vice-versa.
* Zero external dependencies — only Linux kernel headers and a C99 compiler are needed.
* Designed for resource-constrained targets (OpenIPC cameras, routers, embedded SBCs).

---

## Repository layout

```
camex/
├── camexd.c        # Userspace daemon (pure POSIX/Linux, no deps)
├── Makefile        # Builds daemon; optionally builds kmod
└── kmod/
    ├── camex_kmod.c  # Optional kernel module
    └── Makefile      # Out-of-tree kmod Makefile
```

---

## Userspace daemon

### Build

```sh
# Native build
make

# Cross-compile (e.g. for MIPS OpenWrt)
make CROSS_COMPILE=mips-linux-gnu- ARCH=mips
```

The resulting binary `camexd` has no runtime dependencies beyond the standard C library.

### Usage

```
camexd -r <remote_addr> [options]

  -r <addr>   Remote peer IP address (required)
  -q <port>   Remote peer UDP port    (default: 7777)
  -l <addr>   Local bind address      (default: 0.0.0.0)
  -p <port>   Local UDP port          (default: 7777)
  -i <name>   TUN interface name      (default: camex0)
  -m <mtu>    MTU in bytes            (default: 1400)
  -d          Daemonize
  -v          Verbose (log to stderr)
  -h          Show this help
```

### Quick-start example

**Host A** (192.168.1.10):
```sh
# Start tunnel
camexd -r 192.168.1.20 -p 7777 -d

# Configure the TUN interface
ip addr add 10.0.0.1/30 dev camex0
ip link set camex0 mtu 1400 up
```

**Host B** (192.168.1.20):
```sh
camexd -r 192.168.1.10 -p 7777 -d

ip addr add 10.0.0.2/30 dev camex0
ip link set camex0 mtu 1400 up
```

Verify:
```sh
ping 10.0.0.2
```

### Install

```sh
make install PREFIX=/usr/local DESTDIR=
```

---

## Kernel module (optional)

The kernel module provides the same UDP tunnel in kernel space, avoiding
the context-switch overhead of the userspace daemon.

### Build

```sh
# Native build (kernel headers must be installed)
make kmod

# Cross-compile
make kmod ARCH=mips CROSS_COMPILE=mips-linux-gnu- KERNELDIR=/path/to/linux
```

### Load

```sh
# Minimal example — remote peer is 192.168.1.20, UDP port 7777
insmod kmod/camex_kmod.ko remote=192.168.1.20

# Custom port and MTU
insmod kmod/camex_kmod.ko remote=192.168.1.20 port=4444 mtu=1380
```

Module parameters (all read-only after load):

| Parameter | Default | Description                     |
|-----------|---------|---------------------------------|
| `remote`  | —       | Remote peer IPv4 address (**required**) |
| `port`    | `7777`  | UDP port (same for both ends)   |
| `mtu`     | `1400`  | Interface MTU in bytes          |

After loading, configure the `camex0` netdevice as shown above.

### Unload

```sh
rmmod camex_kmod
```

---

## Security notes

* No authentication or encryption is performed — use IPsec, WireGuard, or a
  similar layer on top if confidentiality or integrity is required.
* Restrict the UDP port with firewall rules (`iptables`/`nftables`) to the
  expected peer address.

---

## License

GPL-2.0-or-later — see individual source files.
