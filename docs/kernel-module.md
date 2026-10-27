<!-- Copyright (c) OpenIPC  https://openipc.org  MIT License -->

# Kernel module

## Architecture

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

## Key Design Decisions

### 1. No dependency on `tun.ko`

The upstream `/dev/net/tun` is a separate module (`drivers/net/tun.c`)
that must be either loaded or compiled into the kernel. camex registers
its own `net_device` and `miscdevice` directly, using only core kernel
subsystems (`netdev`, `misc`, `skbuff`) that are **always present** and
cannot be unloaded or disabled.

### 2. `miscdevice` instead of manual `chrdev` registration

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

### 3. Packet queue protected by `spinlock` + `irqsave`

`camex_net_xmit()` is called from softirq context (not from process
context), so `spin_lock_irqsave` is used instead of a mutex to protect
the queue. This prevents deadlocks when called from interrupt context.

### 4. Blocking `read()` via `wait_event_interruptible`

Instead of busy-waiting or timers, the standard kernel mechanism is
used: the process sleeps on a `wait_queue`, and `camex_net_xmit()`
wakes it up via `wake_up_interruptible()`. Signals correctly interrupt
the wait (`-ERESTARTSYS`).

### 5. IPv4-only with explicit validation on `write()`

On `write()`, the first byte of the buffer is checked before
`copy_from_user` via `get_user()`. If the IP version is not 4,
`-EINVAL` is returned without allocating an `skb`, preventing garbage
or non-IPv4 packets from entering the stack.

### 6. Single user (`EBUSY`)

`g_priv->attached` is an `atomic_t` taken with `atomic_cmpxchg()`. If
`/dev/camex` is already open, a subsequent `open()` immediately returns
`-EBUSY`, and two concurrent `open()` calls can never both succeed. On
`close()` the queue is flushed and the slot released under the same lock
that `camex_net_xmit()` takes, so no packet is left for the next `open()`.

### 7. `ARPHRD_NONE` + `IFF_NOARP`

The interface is declared as having no link-layer address. The kernel
does not attempt to resolve MAC addresses (ARP) and does not prepend
Ethernet headers. Userspace sends and receives **bare IPv4 datagrams**
starting at the version byte `0x45...`.

### 8. Compatibility via `#define` shims

All `#if LINUX_VERSION_CODE` guards are grouped into a single `#define`
block at the top of the file. The driver body uses only macro names
(`CAMEX_ALLOC_NETDEV`, `CAMEX_POLL_T`, `CAMEX_SKB_PUT`, etc.) with no
conditional compilation inside functions. This simplifies auditing and
maintenance.

## Kernel Compatibility

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

## Building and Installation

### Prerequisites

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

### Compile

```bash
make kmod
```

### Load the module

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

### Persistent load at boot

```bash
# Install the module into the kernel tree
sudo make kmod-install     # copies camex.ko and runs depmod -a

# Enable autoload
echo "camex" | sudo tee /etc/modules-load.d/camex.conf
```

### Unload

```bash
sudo rmmod camex
```

> Unloading is blocked while `/dev/camex` is held open by any process
> (`rmmod` returns `EBUSY`).

### Building for an embedded kernel (OpenWrt and similar)

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
   `NET_RX_BUSY_POLL`, `NF_CONNTRACK`, `WIRELESS_EXT`. An option turned on by
   `select` goes off only together with the options that select it. An option
   with no prompt (`WIRELESS_EXT`, say) cannot be turned on in `.config` alone:
   `olddefconfig` drops it again unless something selects it; enable an option
   that selects it, or give it `default y` in its `Kconfig`.
3. **Prepare the tree and build** (`<arch>`: the kernel's architecture,
   `mips` for the OpenWrt routers, `arm` for the HiSilicon cameras; `<prefix>`:
   the firmware's toolchain, e.g. `mipsel-openwrt-linux-`):

   ```sh
   make -C <kernel-tree> ARCH=<arch> CROSS_COMPILE=<prefix> olddefconfig modules_prepare
   make kmod KDIR=<kernel-tree> ARCH=<arch> CROSS_COMPILE=<prefix>
   ```

   If the host tools of an older kernel fail to link with GCC 10 or newer
   (`dtc`: multiple definition of `yylloc`), add `HOSTCFLAGS=-fcommon`.
4. **Check the layout before loading.** Build a module the device already has
   from the same tree, for example
   `make -C <kernel-tree> ARCH=<arch> CROSS_COMPILE=<prefix> drivers/net/tun.ko`,
   and compare it with the device's `/lib/modules/$(uname -r)/tun.ko`: the
   `.text` size (`objdump -h`) and the offsets in every load and store
   (`objdump -d`, e.g. `lw v0,452(a0)`) must be the same. Instruction order may
   differ with another build of the same compiler; offsets must not. Without
   such a module on the device, compare one field in the firmware's kernel
   image: disassemble a function that reads it (`register_netdevice` reads
   `dev->netdev_ops`) and compare the offset with
   `offsetof(struct net_device, netdev_ops)` of your tree.
5. **Symptoms of a mismatch.** The handshake and auto-config work, but there is
   no traffic, and `rx_dropped` of `camex` (`cat /proc/net/dev`) grows with
   every packet from the server: stop the client and `rmmod camex` at once. Or
   `insmod` itself ends in a kernel oops (`NULL pointer dereference` in
   `register_netdevice`, from `camex_init`): the module wrote `netdev_ops` where
   the kernel does not read it.

   Since 3.11.1 the module checks the layout itself before it registers the
   interface (#134): `alloc_netdev()` runs in the kernel and sets
   `num_tx_queues`, `real_num_tx_queues` and `gso_max_size`, which lie after
   the config-dependent fields; read through the module's offsets they must
   be 1, 1 and 65536. When they are not, `insmod` fails with
   `Exec format error` and `dmesg` says

   ```
   camex: built for another kernel configuration: struct net_device differs from the running kernel's (...)
   ```

   instead of an oops or a dead interface. It catches a shift before these
   fields, which is what a config difference usually makes, not every
   difference: steps 1 to 4 still apply.

Tested example: OpenWrt 15.05.1 ramips/mt7620 (MT7620N, MIPS 24KEc) with a
custom kernel 3.18.92 — linux 3.18.92 with the patches of `openwrt/chaos_calmer`
at `31cce51cc7` ("kernel: bump to 3.18.92"), the toolchain and `.config` of the
OpenWrt SDK 15.05.1 ramips/mt7620 with `IPV6`, `XFRM` (and `INET_AH`,
`INET_ESP`, `INET_IPCOMP`, `INET_XFRM_MODE_*`, `NET_KEY`, `XFRM_USER`,
`XFRM_IPCOMP`, `XFRM_ALGO`, which select it) and `BRIDGE_NETFILTER` turned off.
With the unchanged SDK config the module loaded but dropped every packet
([#73](https://github.com/OpenIPC/camex/issues/73)).

Tested example: HiSilicon hi3516cv500 with the camera's stock kernel 4.9.37
(zenotech firmware). The HiSilicon SDK kernel tree and the OpenIPC kernel of the
same version build without `WIRELESS_EXT`, the stock kernel has it: its two
pointers in `struct net_device` (8 bytes on ARM32) put `netdev_ops` at 312 in
the running kernel and at 304 in the module, and `insmod camex.ko` oopsed in
`register_netdevice`; a `tun.ko` from that tree loaded and passed no traffic.
With `default y` added to `config WIRELESS_EXT` in `net/wireless/Kconfig`, both
work ([#133](https://github.com/OpenIPC/camex/issues/133)):

```sh
sed -i '/^config WIRELESS_EXT$/a\	default y' net/wireless/Kconfig
```

## Character Device API

| Operation               | Behaviour                                                  |
|-------------------------|------------------------------------------------------------|
| `open()`                | Needs `CAP_NET_ADMIN`; one fd at a time, `EBUSY` otherwise |
| `read()`                | Returns one IPv4 packet. Blocks when queue is empty        |
| `read()` + `O_NONBLOCK` | Returns `EAGAIN` when queue is empty                       |
| `write()`               | Injects one IPv4 packet into the stack. Minimum 20 bytes   |
| `write()` non-IPv4      | Returns `EINVAL`                                           |
| `poll()`                | `POLLIN` when a packet is available; `POLLOUT` always      |
| `close()`               | Flushes the queue, clears carrier, releases the slot       |

### Error codes

| Code       | Cause                                                       |
|------------|-------------------------------------------------------------|
| `EPERM`    | `open()` without `CAP_NET_ADMIN`                            |
| `EBUSY`    | `/dev/camex` is already open by another process             |
| `ENODEV`   | Module is being unloaded                                    |
| `EMSGSIZE` | `read()` buffer smaller than packet length (packet dropped) |
| `EINVAL`   | `write()`: size < 20 or > 65535 bytes, or not IPv4          |
| `EAGAIN`   | `read()` with `O_NONBLOCK`, queue is empty                  |
| `ENOMEM`   | Failed to allocate an `skb`                                 |

## Limitations

### Functional

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

### Queue and Memory

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

### Thread Safety

- Concurrent `read()` calls from multiple threads of the same process
  are not serialised at the fd level — protect access with a mutex in
  userspace.

- Concurrent `write()` calls from multiple threads are safe: each call
  independently allocates its own `skb`.

### Networking

- **No masquerading/NAT out of the box.** To route traffic from
  `camex` to the internet, configure `iptables -t nat -A POSTROUTING`
  manually.

- **`netif_rx()` vs `netif_receive_skb()`.** The driver uses
  `netif_rx()` (the older softirq-deferred path). On high-throughput
  systems, `netif_receive_skb()` offers lower latency but requires
  process or softirq context — the substitution is trivial if needed.

## Comparison with `tun.ko`

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

## Internal Specifics

### Compatibility Shims

All `#if LINUX_VERSION_CODE` guards are placed in a dedicated `#define`
block at the top of `camex-k.c`. The driver body uses only macro names
(`CAMEX_ALLOC_NETDEV`, `CAMEX_POLL_T`, `CAMEX_SKB_PUT`, etc.) with no
version conditionals inside functions. This keeps the logic readable and
easy to audit.

### Registration and Deregistration Order

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

### Carrier and Link State

- `netif_carrier_off` on init — the interface shows `NO-CARRIER` until
  userspace opens `/dev/camex`.
- `netif_carrier_on` on `open()` — carrier becomes active.
- `netif_carrier_off` on `close()` — carrier is cleared; routes through
  the interface become unreachable.

### `GFP_ATOMIC` in `xmit`

`camex_net_xmit()` is called from softirq context where sleeping is
forbidden. Therefore `kmalloc` for `pkt_node` uses `GFP_ATOMIC`. If
memory is unavailable in this context the packet is dropped
(`tx_dropped++`).
