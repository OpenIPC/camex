<!-- Copyright (c) OpenIPC  https://openipc.org  MIT License -->

# Server configuration

The server reads a flat key=value config file, by default
`/etc/camex/camex.conf` ([install.md](install.md)); an example is
[examples/camex.conf](../examples/camex.conf).

```sh
# The key pair first: (umask 077; camex --genkey > /etc/camex/server.key)
sudo camex --mode server --config /etc/camex/camex.conf \
    --server-key-file /etc/camex/server.key
```

Auto-mode clients get their addresses from the config file. A client whose ID
has no `[client ID]` block gets `[defaults]`, or, when no address is configured,
one from the auto pool, `10.200.0.2`–`10.200.0.254` by default (addresses in use
and those of `[client ID]` blocks are skipped; a client that is gone keeps its
address under its ID and gets it back, see
[Denial of service limits](security.md#denial-of-service-limits)). To serve only the IDs
listed in the file, set `allow_unknown_clients=no` in the `[server]` block.
For nginx in front of the clients, `domain=` in `[server]` is the clients'
domain, `name=` in a `[client ID]` block a display name, and `peer_port=` in
`[defaults]` or a `[client ID]` block the port of the start page's link to the
client's external IP (8080 by default, `none` for no link; see
[nginx in front of the clients](nginx.md#reference)).

After a change to the file, `kill -HUP` the server. A running auto-mode client
picks up a changed address, gateway, MTU or routes with its next handshake (a
key renewal, at most 10 minutes later, or a reconnect): it reopens its tunnel
with the new settings and replaces its routes, without a restart. Its old
address keeps working until the client confirms the new one with its first
packet on the new session.

## The configuration file

Three kinds of blocks; lines starting with `#` are comments:

- `[server]` — the server's own settings. Command line arguments take priority
  over them.
- `[defaults]` — the profile of every auto-mode client without its own block.
- `[client ID]` — one client's profile; the ID is its MAC address or any unique
  string. Its keys override `[defaults]`.

`[server]`:

| Key                     | Meaning                                                                                                                                                                 |
|-------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `port`                  | the listening port (`--port`)                                                                                                                                           |
| `bind_ip`               | the address to listen on, `0.0.0.0` by default (`--bind-ip`)                                                                                                            |
| `bind_dev`              | the interface to listen on (`--bind-dev`)                                                                                                                               |
| `local_cidr`            | the server's tunnel address; with the auto pool its first address, e.g. `10.200.0.1/24`                                                                                 |
| `mtu`                   | the tunnel MTU, 1500 by default                                                                                                                                         |
| `server_key`            | the private key from `camex --genkey` (or `--server-key-file`)                                                                                                          |
| `psk`                   | the global pre-shared key: `hex:<64 hex digits>`, `base64:<44 characters>` or a passphrase                                                                              |
| `tun_dev`               | the TUN device or interface name                                                                                                                                        |
| `pid_file`              | write the PID there                                                                                                                                                     |
| `control_socket`        | the control socket of `camex --status`, see below; `none` turns it off                                                                                                  |
| `domain`                | the clients' domain for nginx: `<ID>.<domain>` ([nginx.md](nginx.md))                                                                                                   |
| `client_isolation`      | `yes`: drop packets from one client to another ([security.md](security.md#what-it-does-not-protect))                                                                    |
| `allow_unknown_clients` | `no`: serve only the IDs that have a `[client ID]` block; `yes` by default                                                                                              |
| `auto_pool`             | the auto pool, `10.200.0.0/24` by default, see below                                                                                                                    |
| `max_clients`           | client slots, see below                                                                                                                                                 |
| `client_timeout`        | seconds of silence before a client's session ends and its lamp on the start page turns red, 60 to 120 (default 120; `install.sh` writes 60); a reload applies it (#143) |

`[defaults]` and `[client ID]`:

| Key          | Meaning                                                                                                  |
|--------------|----------------------------------------------------------------------------------------------------------|
| `local_cidr` | the client's tunnel address; every client needs its own                                                  |
| `gateway_ip` | the client's gateway in the tunnel                                                                       |
| `mtu`        | the client's MTU                                                                                         |
| `route_cidr` | a route the client installs through the tunnel; repeatable                                               |
| `psk`        | the client's own pre-shared key; the global one then no longer works for it ([usage.md](usage.md#notes)) |
| `peer_port`  | the port of the start page's link to the client's external IP, 8080 by default, `none` for no link       |
| `name`       | (`[client ID]` only) the display name on the start page and in `camex --status`: UTF-8, up to 64 bytes   |

A `[client ID]` block lists its ID even with only `name=`: with
`allow_unknown_clients=no` that client is let in. With both a global `psk=`
and per-client ones, a client with its own key uses that one, the others the
global one.

## Connected clients (`--status`)

The server listens on a local control socket, by default
`/run/camex/camex-<transport>-<port>.sock` (`udp` or `tcp`; `/var/run/camex` on
macOS), mode `0600`: only the server's user, normally root, can use it. Several
servers on one host (say UDP and TCP) each have their own; a server bound with
`--bind-ip` or `--bind-dev` has the address or interface in the name as well
(`camex-udp-192.0.2.1-5800.sock`). `control_socket=<path>` in `[server]` or
`--control <path>` changes it, `none` turns it off; a reload applies a change,
and keeps the old socket if the new path cannot be had. Not on Windows.

```sh
sudo camex --status          # the clients of every server on this host
sudo camex --status --json   # the same as JSON, for scripts and monitoring
sudo camex --status --control /run/camex/camex-udp-5800.sock   # one server
```

```
camex 3.3.0 server, udp port 5800, tunnel 10.200.0.1/24, pool 10.200.0.0/24, up 2h05m, clients 2, slots 2 of 256

ID     MODE    KEY     ADDRESS          PEER                CONNECTED  LAST       HANDSHAKE     HS          RX   RX PKTS          TX   TX PKTS  NAME
CAM-A  auto    global  10.200.0.2       203.0.113.7:50997   2h04m      3s         4m10s         13     1.2 MiB      9120     8.4 MiB     12480  Warehouse, entrance
CAM-B  auto    own     10.200.0.3       198.51.100.4:41022  12m03s     1s         2m02s          2   310.5 KiB      2210     2.0 MiB      3105

Addresses kept for clients that are gone (#81): 1
CAM-C  10.200.0.4       gone 6m40s
```

`ADDRESS` shows `old -> new` while a client has not confirmed a new address
yet. `LAST` is the time since its last data packet (keepalives included; `-`
before the first), `HANDSHAKE` since its last key renewal, `HS` the number of
handshakes; `RX` is the tunnel data received from it, `TX` the data sent to it,
in bytes and packets. `NAME`, shown when a client has one, is `name=` of its
`[client ID]` block.
The kept addresses are those of clients that are gone; each gets its address
back when it returns.

`--json` prints `{"servers":[...]}`, one object per server: `version`, `pid`,
`transport`, `port`, `tunnel`, `pool`, `started` (Unix time), `uptime`,
`client_isolation`, `isolation_drops` (#83),
`slots` (`used`, TCP connections before their handshake included, and `max`),
`clients` and `leases`. Each client has `id` (`null`
for a manual client without one), `name` (its `name=`, or `null`), `mode` (`auto` or `manual`), `key` (`global`
or `own`), `address`, `pending` and `previous` (or `null`), `peer`,
`transport`, `registered` (Unix time of its first handshake), `connected_s` (its
current connection), `last_packet_s` (`null`
before the first data packet),
`last_handshake_s`, `handshakes`, `rx_packets`, `rx_bytes`, `tx_packets`,
`tx_bytes`, `isolation_drops`; each lease `id`, `address`, `gone_s` and `previous` (`true` for
the address of the client's previous session).


## Auto pool and number of clients

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
  startup only; a reload that asks for another number says a restart is
  needed. Over TCP the server uses `select()` and keeps at most `FD_SETSIZE`
  − 32 slots (992 on Linux) and no more than the open files limit allows (it
  raises its soft limit towards the hard one); the log says so.

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
swapped out; give it a memory lock limit for its slots and configuration (root has none;
otherwise `ulimit -l`, `LimitMEMLOCK=infinity` in systemd, `--cap-add IPC_LOCK`
or `--ulimit memlock=-1` in Docker), or it runs unlocked with a `SECURITY
WARNING` in the log. With thousands of `[client ID]` blocks, give their `psk=`
as `hex:` or `base64:` keys: each passphrase costs an Argon2id run (8 MiB, tens
of milliseconds) when the server starts, and on reloads beyond the 260 keys it
caches. A handshake tries the keys in turn (the global key first, always), at
most 51200 tries of per-client keys a second for all handshakes together, or
four times their number when there are more: per-client keys suit tens or hundreds
of clients; for thousands give them the global key (they are still told apart
by their ID) and per-client keys only where needed.

The session index in each packet carries the client's slot number in its low
16 bits (the high 16 bits are random), so the server finds a packet's session
without a search.
