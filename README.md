# Camex — v2.0.0

Minimal dependency-free Linux UDP/TUN tunnel for embedded targets.

Ships as a userspace daemon and an optional standalone kernel module.

## Architecture

| Component | Description |
|---|---|
| `camex` (userspace) | Client/server daemon that creates a TUN interface, encapsulates IPv4 packets in UDP (with TCP helper functions available), and optionally encrypts them with ChaCha20-Poly1305. |
| `camex.ko` (kernel module) | Standalone TUN driver that registers `/dev/camex` and a `camex` network interface — no dependency on `tun.ko`. |
| Relationship | The userspace daemon can use **either** the standard `/dev/net/tun` (tun.ko) **or** `/dev/camex` (camex.ko) as its TUN backend. Both produce bare IPv4 packets; the tunnel protocol is identical. |

## Requirements

| Requirement | Details |
|---|---|
| Linux kernel | 3.10 or later |
| TUN support | `CONFIG_TUN=y` (`/dev/net/tun`) **or** `camex.ko` loaded (`/dev/camex`) |
| Privileges | `root` or `CAP_NET_ADMIN` + `CAP_NET_RAW` |
| C compiler | C99-compatible (GCC or Clang) |

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

| Feature | tun.ko (`/dev/net/tun`) | camex.ko (`/dev/camex`) |
|---|---|---|
| Interface name | dynamic (`tun0`, `tun1`, …) | always `camex` |
| Requires TUNSETIFF ioctl | Yes | No |
| External dependency | `CONFIG_TUN=y` or `tun.ko` loaded | `camex.ko` loaded |
| IPv4 | Yes | Yes |
| IPv6 | Yes | No (IPv4 only) |
| Multiple instances | Yes | No (one) |
| Bare IPv4 packets | Yes (with `IFF_NO_PI`) | Yes |

## Userspace App

### Modes

| Mode | Description |
|---|---|
| `client` | Creates a TUN interface and connects to the server |
| `server` | Listens for many clients and forwards tunnel packets |

### Usage

#### Client

```sh
sudo ./camex \
  --mode client \
  --auto \
  --name 0203A104B5AE \
  --server-host vpn.example.org \
  --port 7000 \
  --encrypt --psk secret
```

Manual client mode:

```sh
sudo ./camex \
  --mode client \
  --local-cidr 10.0.0.2/24 \
  --gateway-ip 10.0.0.1 \
  --server-host vpn.example.org \
  --port 7000 \
  --route-cidr 192.168.100.0/24 \
  --encrypt --psk secret
```

With explicit TUN backend:

```sh
sudo ./camex \
  --mode client \
  --local-cidr 10.0.0.2/24 \
  --gateway-ip 10.0.0.1 \
  --server-host vpn.example.org \
  --port 7000 \
  --tun-dev /dev/camex
```

#### Server

```sh
sudo ./camex \
  --mode server \
  --port 7000 \
  --bind-ip 0.0.0.0 \
  --config /etc/camex/camex.conf \
  --encrypt --psk secret
```

### Options

| Option | Short | Description |
|---|---|---|
| `--mode <mode>` | `-M` | `client` or `server` |
| `--auto` | `-a` | Fetch tunnel parameters from the server |
| `--name <id>` | `-n` | Client ID used in auto mode |
| `--local-cidr <cidr>` | `-l` | Tunnel address in `address/prefix` form |
| `--gateway-ip <addr>` | `-g` | Client gateway inside the tunnel |
| `--server-host <addr>` | `-s` | Server hostname or IP for client mode |
| `--port <port>` | `-p` | Server port for client or listen port for server |
| `--bind-ip <addr>` | `-b` | Optional server bind address |
| `--config <path>` | `-f` | Server config file path |
| `--route-cidr <cidr>` | `-c` | Extra route to install on the client; repeatable |
| `--mtu <size>` | `-t` | Tunnel MTU, 576–9000 |
| `--psk <passphrase>` | `-k` | Passphrase stretched into a 32-byte key |
| `--encrypt` | `-e` | Enable ChaCha20-Poly1305 transport |
| `--tun-dev <path>` | `-T` | TUN device path (default: auto-detect `/dev/net/tun` then `/dev/camex`) |
| `--pid-file <path>` | `-P` | Write PID to file on startup |
| `--bind-dev <iface>` | `-d` | Bind socket to a specific network interface (`SO_BINDTODEVICE`) |
| `--transport <proto>` | `-R` | Transport protocol: `udp` (default) or `tcp` |
| `--version` | `-v` | Print version and exit |
| `--help` | `-h` | Print usage and exit |

## Server Configuration

The server reads a flat key=value config file with one client record per block.
See `camex.conf.example` for the full format.

```sh
sudo ./camex --mode server --port 7000 --config /etc/camex/camex.conf
```

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
- sets permissions to `0666` (readable and writable by all users).

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

`g_priv->attached` is a boolean flag. If `/dev/camex` is already open,
a subsequent `open()` immediately returns `-EBUSY`. This prevents a
race condition between two processes writing to the same queue.

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

| Range          | Status | Adapted API                                              |
|----------------|--------|----------------------------------------------------------|
| 3.10 – 3.16    | ✅     | `alloc_netdev(sz, name, setup)` — 3 arguments            |
| 3.17 – 3.19    | ✅     | `alloc_netdev` + `NET_NAME_PREDICTABLE` — 4 arguments    |
| 4.0  – 4.9     | ✅     | `min_mtu`/`max_mtu` fields absent from `net_device`      |
| 4.10 – 4.15    | ✅     | `min_mtu`/`max_mtu` fields introduced                    |
| 4.16 – 4.19    | ✅     | `poll` returns `__poll_t`; `EPOLL*` constants            |
| 4.20+          | ✅     | `skb_put()` returns `void*` instead of `unsigned char*`  |
| 5.x            | ✅     | no changes required                                      |
| 6.0 – 6.6 LTS  | ✅     | no changes required                                      |

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
# crw-rw-rw- 1 root root 10, 58 ...  /dev/camex

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

### Character Device API

| Operation               | Behaviour                                                         |
|-------------------------|-------------------------------------------------------------------|
| `open()`                | Only one fd allowed at a time; returns `EBUSY` otherwise          |
| `read()`                | Returns one IPv4 packet. Blocks when queue is empty               |
| `read()` + `O_NONBLOCK` | Returns `EAGAIN` when queue is empty                              |
| `write()`               | Injects one IPv4 packet into the stack. Minimum 20 bytes          |
| `write()` non-IPv4      | Returns `EINVAL`                                                  |
| `poll()`                | `POLLIN` when a packet is available; `POLLOUT` always             |
| `close()`               | Flushes the queue, clears carrier, releases the slot              |

#### Error codes

| Code       | Cause                                                        |
|------------|--------------------------------------------------------------|
| `EBUSY`    | `/dev/camex` is already open by another process              |
| `ENODEV`   | Module is being unloaded                                     |
| `EMSGSIZE` | `read()` buffer smaller than packet length (packet dropped)  |
| `EINVAL`   | `write()`: size < 20 or > 65535 bytes, or not IPv4           |
| `EAGAIN`   | `read()` with `O_NONBLOCK`, queue is empty                   |
| `ENOMEM`   | Failed to allocate an `skb`                                  |

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

| Feature                           | camex              | tun.ko               |
|-----------------------------------|--------------------|----------------------|
| External module dependency        | No                 | Requires tun.ko      |
| IPv4                              | Yes                | Yes                  |
| IPv6                              | No (intentional)   | Yes                  |
| TAP (L2)                          | No                 | Yes                  |
| Multiple device instances         | No (one)           | Yes (dynamic)        |
| Multiple open file descriptors    | No (one)           | Yes                  |
| ioctl TUNSETIFF / TUNGETIFF       | No                 | Yes                  |
| Persistent device                 | No                 | Yes                  |
| poll / select / epoll             | Yes                | Yes                  |
| O_NONBLOCK                        | Yes                | Yes                  |
| Statistics (`ip -s link`)         | Yes                | Yes                  |
| Kernel 3.x support                | Yes                | Yes                  |
| Code size                         | ~490 lines         | ~3500 lines          |

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

## Notes

- Each client should use a unique tunnel IP.
- The server relays packets based on the inner IPv4 destination address.
- Client sockets are outbound only; no listening port is opened on the client side.
- In auto mode, `-l`, `-g`, and `-c` are supplied by the server.
- `camex.conf.example` shows the server-side parameter database format.
