<!-- Copyright (c) OpenIPC  https://openipc.org  MIT License -->

# Usage

The camex command line: modes, keys, client and server, every option, the TUN
backends. Installing the server on Debian or Ubuntu: [install.md](install.md);
the server's configuration file: [server.md](server.md).

## Keys

The server has a key pair; every client needs the server's **public** key and
a pre-shared key (the global one, or its own `psk=` from the server config).

```sh
(umask 077; camex --genkey > server.key)   # server_key=… (secret) and public_key=…
```

## Modes

| Mode     | Description                                          |
|----------|------------------------------------------------------|
| `client` | Creates a TUN interface and connects to the server   |
| `server` | Listens for many clients and forwards tunnel packets |

## Command lines

### Client

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

### Server

```sh
sudo ./camex \
  --mode server \
  --port 5800 \
  --bind-ip 0.0.0.0 \
  --config /etc/camex/camex.conf \
  --server-key-file /etc/camex/server.key \
  --psk secret
```

## Options

| Option                     | Short | Description                                                                                                                                                                                                                                                                 |
|----------------------------|-------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `--mode <mode>`            | `-M`  | Operation mode: `client` or `server` (required)                                                                                                                                                                                                                             |
| `--auto`                   | `-a`  | Fetch tunnel parameters from the server                                                                                                                                                                                                                                     |
| `--bridge <iface>`         |       | Client of a TAP network (the full camex): bridge it to this LAN interface, no bridge or TAP device ([networks.md](networks.md#bridging-a-lan---bridge)) |
| `--bridge-mode <m>`        |       | With `--bridge`: `auto` (default: `nat` on Wi-Fi, else `full`), `full` or `nat` |
| `--network <name>`         |       | Client: join the server's network of this name ([networks.md](networks.md)); the name is the network's key, so `--psk` is not needed (given too, it is the client's own key). Implies `--auto` |
| `--name <id>`              | `-n`  | Client ID (auto mode). Without it: the MAC of the default route's interface, kept in `/tmp/camex-id` until a reboot                                                                                                                                                         |
| `--local-cidr <cidr>`      | `-l`  | Tunnel address in `address/prefix` form                                                                                                                                                                                                                                     |
| `--gateway-ip <addr>`      | `-g`  | Client gateway inside the tunnel                                                                                                                                                                                                                                            |
| `--server-host <addr>`     | `-s`  | Server hostname or IP for client mode                                                                                                                                                                                                                                       |
| `--port <port>`            | `-p`  | Server port for client (default: 5800) or listen port for server                                                                                                                                                                                                                            |
| `--bind-ip <addr>`         | `-b`  | Server bind address (default: 0.0.0.0)                                                                                                                                                                                                                                      |
| `--config <path>`          | `-f`  | Server config file path                                                                                                                                                                                                                                                     |
| `--route-cidr <cidr>`      | `-c`  | Extra route to install on the client; repeatable                                                                                                                                                                                                                            |
| `--mtu <size>`             | `-t`  | Tunnel MTU, 576–9000 (default: 1420; TAP: 1500)                                                                                                                                                                                                                             |
| `--psk <key>`              | `-k`  | Pre-shared key: `hex:<64 hex digits>`, `base64:<44 chars>`, or a passphrase (stretched with Argon2id). Required on clients without `--network`                                                                                                                                                 |
| `--server-pubkey <key>`    |       | Client: the server's public key (from `--genkey`); without it the key the server gives on first contact is trusted and kept ([networks.md](networks.md#the-servers-key))                                                                                                                                                                                                                          |
| `--known-servers <path>`   |       | Client: where the keys of servers met without `--server-pubkey` are kept (default: `/etc/camex/known_servers`; `none`: not kept) |
| `--server-key-file <path>` |       | Server: file with its private key (or `server_key=` in `[server]`)                                                                                                                                                                                                          |
| `--no-tun-offload`         |       | Linux: no TUN offloads (see [TUN driver support](#tun-driver-support)); for a kernel that gets them wrong                                                                                                                              |
| `--bench-crypto`           |       | How fast this CPU seals 1400-byte tunnel packets (ChaCha20-Poly1305, 3 s), and whether NEON is used; then exit |
| `--genkey`                 |       | Print a new server key pair (`server_key=`, `public_key=`) and exit                                                                                                                                                                                                         |
| `--encrypt`                | `-e`  | Accepted for compatibility; protocol v2 is always encrypted                                                                                                                                                                                                                 |
| `--tun-dev <path>`         | `-T`  | TUN device path (default: auto-detect `/dev/net/tun` then `/dev/camex`)                                                                                                                                                                                                     |
| `--pid-file <path>`        | `-P`  | Write PID to file on startup                                                                                                                                                                                                                                                |
| `--keepalive <s>`          | `-K`  | Client: keepalive interval, 0–3600 seconds (default: 10); at most 20 s (#143); 0: one every 20 s, a lost link noticed after 20 s + `--server-timeout`                                                                                                                       |
| `--web-port <port>`        |       | Auto client: the port of its web UI, for nginx on the server (#174); needs a server 3.19.0 or later, an older one refuses the client with it                                                                                                                                |
| `--server-timeout <s>`     | `-W`  | Client: seconds without an answer from the server before it reconnects, 5–3600 (default: 20)                                                                                                                                                                                |
| `--status`                 |       | Print the connected clients of every server on this host and exit (as the server's user, normally root)                                                                                                                                                                     |
| `--top`                    |       | A live view of the clients of every server on this host (or of `--control`), busiest first, with their traffic rates; `q` quits ([networks.md](networks.md#status)) |
| `--interval <s>`           |       | With `--top`: seconds between updates, 1–60 (default: 1) |
| `--json`                   |       | With `--status`: the same as JSON, for scripts                                                                                                                                                                                                                              |
| `--control <path>`         |       | Server: control socket for `--status` (default `/run/camex/camex-<transport>-<port>.sock`, `<transport>` is `both`, `udp` or `tcp`; `none` = off); with `--status`: ask only that server                                                                                    |
| `--format <fmt>`           |       | With `--status`: `table` (default), `json` (same as `--json`), `nginx-map` or `nginx-index`, see [nginx in front of the clients](nginx.md#reference)                                                                                                                        |
| `--domain <name>`          |       | With `--format nginx-map` or `nginx-index`: the domain after the client ID (default: `domain=` in `[server]` of the server)                                                                                                                                                 |
| `--upstream-port <port>`   |       | With `--format nginx-map`: the clients' web port (default: 80)                                                                                                                                                                                                              |
| `--http-port <port>`       |       | With `--format nginx-index`: nginx's port, in the links to the devices (default: 80, none in the link)                                                                                                                                                                      |
| `--default <ip:port>`      |       | With `--format nginx-map`: where names of no live client go (default: none, nginx's offline page)                                                                                                                                                                           |
| `--wait <token>`           |       | With `--status`: wait until the clients of any server (or of `--control`) change, then print the token of the new state (`none`: no server); `-` prints it at once. For `camex-nginx-sync --follow` ([nginx.md](nginx.md#sync-script)); exit code 3: a server before 3.11.0 |
| `--wait-timeout <s>`       |       | With `--wait`: the longest wait, 1–3600 seconds (default: 60); then the same token                                                                                                                                                                                          |
| `--client-isolation`       |       | Server: drop packets from one client to another (or `client_isolation=yes` in `[server]`), see [Security Model](security.md)                                                                                                                                                |
| `--bind-dev <iface>`       | `-d`  | Bind socket to a specific network interface (`SO_BINDTODEVICE`)                                                                                                                                                                                                             |
| `--transport <proto>`      | `-R`  | Transport: `udp` or `tcp`; a server also `both`, its default (UDP and TCP on the same port, one pool), a client `udp` by default                                                                                                                                            |
| `--version`                | `-v`  | Print version and exit                                                                                                                                                                                                                                                      |
| `--help`                   | `-h`  | Print usage and exit                                                                                                                                                                                                                                                        |

## TUN driver support

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
| Offloads (TSO, GRO)      | Yes (`IFF_VNET_HDR`)              | No                      |

**Offloads.** On Linux, a TUN device camex makes itself through `/dev/net/tun`
takes offloads (`IFF_VNET_HDR`, `TUNSETOFFLOAD`): the kernel hands camex TCP
data of up to 64 KB at once and checksums still to compute, and camex cuts it
into packets of the tunnel's MTU; TCP packets of one connection that arrive
together are written as one, as the kernel's GRO joins them. Nothing changes on
the wire, so old peers are not affected. The log says `TUN offloads: on`.
Between two x86 hosts this carries 19–37% more TCP; on a camera (Cortex-A7)
it saves a little CPU, since the UDP packets and their encryption cost the
most there. Not used for TAP networks, `/dev/camex`, a device given from
outside (Android, `CAMEX_TUN_FD`), macOS or Windows. `--no-tun-offload` turns
them off.

## Notes

- Each client should use a unique tunnel IP.
- The server relays packets based on the inner IPv4 destination address.
- Client sockets are outbound only; no listening port is opened on the client side.
- In auto mode, `-l`, `-g`, and `-c` are supplied by the server.
- A client started before the network is ready waits for it (#121): without
  `--name` for a default route to take the ID from, then for the server as long
  as it takes, with a new socket (and the name resolved again) every
  `--server-timeout` without an answer, so a changed address or route is picked
  up. The ID taken from the MAC is kept in `/tmp/camex-id` until a reboot, so a
  restart keeps it whatever the network looks like then. With `--keepalive 0` a
  keepalive still goes every 20 s, and a lost server is noticed after 20 s +
  `--server-timeout`.
- `examples/camex.conf` shows the server-side parameter database format.
- Protocol v2 works over both UDP and TCP transport; a server listens on both
  on one port unless given `--transport udp` or `tcp` (#178), a client uses UDP
  unless given `--transport tcp`, and never moves to TCP by itself. Every
  session starts with
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
