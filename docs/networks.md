<!-- Copyright (c) OpenIPC  https://openipc.org  MIT License -->

# Networks

A client joins a network with three things: the server's address, the
network's name and its own ID. The ID is the MAC of the default route's
interface unless `--name` gives one. The server derives the client's address
from the network and the ID. The client gets the same address after a client
restart, a server restart or a move to another server with the same file.

```sh
camex --server-host vpn.example.org --network "field cameras"
```

The client defaults are client mode, auto mode and port 5800. Without
`--server-pubkey`, the client trusts the key the server gives on first
contact (see [The server's key](#the-servers-key)).

## The name is the key

The network's name is its secret: whoever knows it can join. Choose a name
that is hard to guess, like a password.

- The client never sends the name, nor anything an attacker could test
  guesses against offline. It and the server run CPace first: a
  password-authenticated key exchange on the name (see [The server's
  key](#the-servers-key)). The key CPace gives is the pre-shared key of the
  Noise handshake that follows.
- The server never logs the name. Logs and `camex --status` show `label=`
  or `net-` plus the first 4 bytes of the ID in hex.
- The client hides `--network` from `ps` by overwriting it in its argument
  list.

Extra protection on top of the name, all optional:

| What                                    | Stops                                                                      |
|-----------------------------------------|----------------------------------------------------------------------------|
| `--server-pubkey` on the client         | a fake server at the very first contact with a server that takes new networks (see below) |
| `psk=` in the client's `[client ID]`    | someone who knows the name posing as that client ID; the client then needs `--psk` with that key besides `--network` |
| `client_isolation = yes` in `[network]` | clients of the network reaching each other                                 |
| `allow_unknown_clients = no`            | client IDs without a `[client ID]` block                                   |

## The server's file

```ini
[server]
local_cidr = 10.200.0.1/24       # needed: the networks' gateways go on this tunnel
# allow_new_networks = yes       # any name makes a new network (off by default)
# network_supernet = 10.0.0.0/8  # where derived pools come from (the default)
# network_prefix = 16            # their size (the default)

[network field cameras]
label = cams                     # shown instead of the name in logs and status

[network lab]
pool = 10.77.0.0/24              # this pool instead of a derived one
client_isolation = yes
```

`[network NAME]`: NAME runs from after `network` to the `]`, without the
spaces around it. It is 1 to 63 bytes and case-sensitive.

| Key                | Meaning                                                                                   |
|--------------------|-------------------------------------------------------------------------------------------|
| `pool`             | the network's IPv4 pool; without it a `/16` of `network_supernet` picked by the network's ID |
| `client_isolation` | `yes` or `no`: drop packets between clients of this network; without it the server's setting |
| `label`            | the name shown in logs and in `camex --status`, up to 31 bytes without spaces              |

`[server]`:

| Key                  | Meaning                                                                                       |
|----------------------|-----------------------------------------------------------------------------------------------|
| `allow_new_networks` | `yes`: a client may name any network; the server makes it on its first handshake (see below)   |
| `network_supernet`   | the range derived pools come from, `10.0.0.0/8` by default                                    |
| `network_prefix`     | the size of a derived pool, 9 to 30, `16` by default                                          |

The file is read again when it changes. The server checks its time, size and
inode every 2 seconds, so no `kill -HUP` is needed, though it still works. A
file that does not parse, or that gives a pool overlapping another one, is not
applied: the server keeps its configuration and logs why.

A server with networks needs no global `psk=`: clients of networks use none.
Clients with `--psk`, and manual clients, work as before next to the
networks.

## Addresses

Every network's pool is laid out the same way:

- the first address is the network's gateway. The server puts it on its tunnel
  interface as an alias (`camex0:<slot>`), which also adds the route to the
  pool;
- clients get the second address to the last but one.

A client's address is, in this order:

1. `local_cidr=` of its `[client ID]` block, if that is in the network's pool
2. the address it already holds: a live session, or a lease (kept for an
   ID whose client is gone, as for the auto pool, #81)
3. the address it had before a server restart, which the client asks for
4. the hash's choice: BLAKE2b of the network's ID and the client ID (any
   case), modulo the pool's client addresses
5. if another client ID holds that one: +1, −1, +2, −2 … around it, the first
   free address. The server logs `Network <label>: <address> of client <ID>
   is taken by client <ID>, it gets <address>`.

An address is only taken when another client ID holds it. A client that comes
back with the same ID always gets its own address, even while its old session
still looks live to the server: a reboot, a crash, a new connection.

With a `/16` pool and 20 clients, two IDs share a first choice with a
probability of about 0.3%. With a `/24` it is about 50%; `pool=` can set a
`/24` anyway. When two IDs collide, the one that comes later gets the next
address and keeps it while the first one's address is held. After a server
restart, both clients ask for the addresses they had, so they keep their
places too.

## Networks never meet

The server drops a packet from a client of one network to a client or
address of another network, the auto pool included. Packets from the
tunnel interface to a client are dropped when their source is in another
network's pool, so a host that routes between interfaces cannot join them
either. `isolation_drops` in `camex --status --json` counts the drops.

## `allow_new_networks`

With `allow_new_networks = yes`, the server takes a network ID it does not
know as a new network. It gets a derived pool, a `net-` label and its gateway
on the tunnel, and it is listed with `(new)` in `camex --status`. Such
networks live until the server stops, or until a reload turns
`allow_new_networks` off. At most 256 networks exist at once, listed ones
included.

Anyone who knows the server's address can make networks then, so turn it on
only on a server meant to be open.

## The server's key

A client with `--network` and without `--server-pubkey` (or `--psk`) checks
the server by the network's name at every connection:

1. **KEYREQ.** It asks for the server's public key: 64 bytes, answered with
   37.
2. **CPace.** `PAKE1` (1200 bytes, padded so no answer is larger) carries the
   client's share, made on a point derived from the network's ID. `PAKE2` has
   one answer for each network the server knows (at most 24), each with a tag
   that only the right name opens. That tag also covers the server's key.
3. **Confirm.** The client takes the answer whose tag opens, confirms with
   `PAKE3`, and uses the shared key as the Noise pre-shared key. Its REGISTER
   carries no network ID: the server knows the network from CPace.

So a server that does not know the name cannot complete the handshake,
whatever key it shows. An attacker in the middle gets one guess per
connection at most, and nothing to test guesses against offline.

The key, once proven this way, is kept in the known servers file with a
`pake` mark:

- `/etc/camex/known_servers` by default; `--known-servers <path>` sets
  another path, `--known-servers none` keeps nothing;
- one `<host>:<port> <base64 key> pake` line per server, rewritten through
  a new file so a power cut leaves either the old or the new one.

When the server gets a new key, its clients prove it by the name at their next
connection and keep the new one (`proved it knows the network (CPace); its
key ... is kept`). Nobody has to remove anything.

**Networks the server does not know yet** (`allow_new_networks = yes`) are the
exception. The server cannot know a name it never heard, so a new network is
still created the old way: the network's ID goes inside Noise message 1, and
the server's key is trusted on first use (kept without the mark). The same
fallback applies to a server too old for CPace, after three unanswered
`PAKE1`. Both fallbacks happen only while the file has no `pake` mark for that
server. A server that once proved it knows the network and then says it does
not know it but takes new ones is refused as an impostor (`not trusted (an
impostor?)`).

In short:

- listed networks are protected at every contact, the first one included;
- creating a new network on an open server is protected only by trust on first
  use;
- `--server-pubkey` closes that gap too.

## Status

`camex --status` lists the networks in its header, each with its label, pool
and number of clients. `camex --status --json` adds `networks` (`label`,
`pool`, `listed`, `clients`, `client_isolation`), `allow_new_networks`, and
for each client `network` and its traffic rates `rx_rate` and `tx_rate` in
bytes a second.

`camex --top` is redrawn in place every second (`--interval <s>` sets the
interval; `q` quits). A summary first, then a block for each network: its
clients, the busiest first, and its mesh pairs.

```
camex 4.6.0 server   up 49s   UDP+TCP port 5800   tunnel 10.201.0.1/24   19:19:17
Clients 3 of 256   In 0.7 Mbit/s   Out 0.7 Mbit/s   Relayed 0.0 Mbit/s

NETWORK cams  10.77.0.0/24, mesh
  CLIENT     ADDRESS          ENDPOINT          VIA  IN Mbit/s  OUT Mbit/s          IN         OUT   SEEN
  CAM-FRONT  10.77.0.84       172.29.0.3:48265  UDP          -           -       419 B        84 B     8s
  CAM-BACK   10.77.0.5        172.29.0.4:33070  UDP          -           -       401 B        84 B     9s
  Mesh  CAM-BACK  <-> CAM-FRONT  direct    3.9 Mbit/s

NETWORK office  10.78.0.0/24
  CLIENT     ADDRESS          ENDPOINT          VIA  IN Mbit/s  OUT Mbit/s          IN         OUT   SEEN
  LAPTOP-7   10.78.0.243      172.29.0.5:45132  TCP        0.7         0.7     3.1 MiB     3.1 MiB     0s
```

`IN` is what the client sends the server, `OUT` what the server sends it
(rates in Mbit/s, `-` when idle; totals since it connected); `SEEN` the time
since its last packet. Clients without a network follow in a `CLIENTS` block
of the auto pool.

## The full camex

Three features are built only into the full camex: TAP networks, bridging a
LAN, and mesh networks. Each release has both binaries: `camex-<arch>`, the
core as before, and `camex-<arch>-full`. To build the full one:

```sh
make CAMEX_L2=1 CAMEX_MESH=1        # both; either flag alone works too
```

A core camex refuses what it was built without, and says so:

- a server refuses `mode = tap` or `mesh = yes` in its file;
- a client refuses a server's `MODE=TAP`;
- a client built without mesh never asks for it, and its network's other
  clients reach it through the server.

## TAP networks (`mode = tap`)

```ini
[network lan]
mode = tap
pool = 10.66.0.0/24
```

A TAP network carries Ethernet frames, not IPv4 packets. Each client gets a
TAP interface (`tap0`) with a MAC the server derives from the network and its
ID. The MAC is locally administered and the same after any restart. The
client also gets its address from the pool as in a TUN network.

- The server is the network's switch. It learns which client a MAC is behind
  and sends a frame only there. It floods broadcast, multicast and unknown
  destinations to the network's other clients; with `client_isolation` it
  floods nothing and drops frames between clients.
- The server is also the network's gateway. It answers ARP for the gateway
  address with the gateway's MAC, and IPv4 sent to that MAC goes to its tunnel
  interface.
- MTU 1500: camex carries a frame longer than 1300 bytes in pieces of its own
  (up to 8), each in its own packet. A full 1514-byte frame never needs an IP
  fragment on the way, which NATs and mobile networks often drop; the largest
  datagram is about 1340 bytes.
- TAP clients run on Linux only, with `/dev/net/tun`, or with `camex.ko`
  loaded with `mode=tap` ([kernel-module.md](kernel-module.md)). Android
  (`VpnService`), macOS and Windows have no TAP devices.

`addresses = none` gives the clients no addresses: the TAP interface comes up
without one, and the server is only the switch, with no gateway. Use it for a
network bridged to a LAN, whose devices take addresses from the LAN, e.g. with
`udhcpc -i tap0`.

## Bridging a LAN (`--bridge`)

```sh
camex --server-host vpn.example.org --network site --bridge eth0
```

A client of a TAP network can bridge it to one of its interfaces. The remote
devices are then on that LAN as if plugged into it: DHCP from the LAN works,
and they reach the LAN's devices and are reached by them. The host keeps its
own settings and goes on reaching everything:

- **No bridge device, no TAP device.** A packet socket on the interface sees
  every frame there, including the host's own outgoing ones, and sends frames
  onto it. Promiscuous mode is asked through that socket, and the kernel drops
  it when camex stops; the interface's flags, addresses and offloads are left
  alone.
- **LAN → remote devices:** broadcast, multicast, and frames to a MAC that
  came from the tunnel go into the tunnel; the rest stays on the LAN.
- **Host ↔ remote devices:** a frame sent through the packet socket never
  reaches the host's own stack, so camex hands IPv4 packets for the host to it
  through a raw IP socket. It answers ARP for the host's address, and gives the
  host permanent neighbour entries for the remote devices, which it removes
  when it stops.
- **Offloads:** the kernel says which frames still need their checksum
  (checksum offload) and which are TCP segments merged by TSO or GRO
  (`PACKET_VNET_HDR`). camex completes the checksums and cuts the merged
  segments into frames of the interface's MTU.
- **Loops:** every bridge sends a probe frame (EtherType `0x88B5`) on its LAN
  every 5 s. A bridge that sees another bridge of the same network there with
  a lower client ID stops bridging, and says so, until that one has been
  silent for 15 s.

`--bridge-mode` sets how frames go onto the LAN:

| Mode   | When                                                   | What passes                                     |
|--------|--------------------------------------------------------|-------------------------------------------------|
| `full` | Ethernet, a Wi-Fi access point or 4-address (WDS) link | every frame, as it is                           |
| `nat`  | a Wi-Fi client (station)                               | IPv4 and ARP, with the host's MAC towards the LAN |
| `auto` | the default                                            | `nat` on a Wi-Fi interface, `full` elsewhere    |

A Wi-Fi access point takes frames only from the MACs associated with it, so
over a Wi-Fi station link the bridge works like OpenWrt's `relayd`:

- remote devices' frames go onto the LAN with the host's MAC, ARP included;
- frames to the host's MAC whose IPv4 destination is a remote device go to
  that device;
- DHCP requests keep their client hardware address and ask for broadcast
  answers, so every remote device still gets its own lease.

Only IPv4 and ARP pass. If the host forwards IPv4 (`ip_forward=1`), it also
routes the packets meant for remote devices; camex warns about it.

## Mesh networks (`mesh = yes`)

```ini
[network cams]
mesh = yes
```

In a mesh network, clients talk to each other directly wherever their NATs
let them, and through the server where they cannot. It works for TUN and TAP
networks alike.

- **Peer socket.** Every mesh client opens a second UDP socket for its peers.
  It sends an `ENDPOINT` packet from it to the server, sealed with its session
  so nobody else can claim it, every 20 s. The server learns that socket's
  address and port after the client's NAT. The client also tells the server
  the socket's address on its own LAN.
- **Asking for a peer.** Traffic to another client of the network goes through
  the server at first, as always. With it, the client asks the server about
  that peer in a control message inside its session. The server answers both
  clients at once with each other's two endpoints and a fresh key for the
  pair.
- **Punching.** Both clients send punches to each other's public and LAN
  endpoints every 250 ms for 10 s. That opens their NATs to each other. The
  first authenticated answer makes the path direct, and the client logs `Mesh:
  peer <ID> direct at <address>`. Peer-to-peer packets are sealed with the
  pair's keys (ChaCha20-Poly1305, one key and counter per direction, with a
  replay window).
- **Falling back.** A direct path without a packet for 35 s goes back through
  the server (`back through the server`) and is tried again 5 s later. One that
  never opens stays on the server (`stays on the server: no answer to
  punches`) and is tried again after 60 s. Traffic never stops either way.
- **Keepalives.** A direct path gets one every 15 s while it carries nothing.
- **Without the server.** Direct paths do not need it: their keys and
  keepalives are the peers' own. When the server stops (or is restarted),
  clients log `Connection lost` and keep reconnecting, while peers with a direct
  path go on talking. What needs the server waits for it: the server itself,
  relayed peers, peers not yet met, and a peer's new address after it moved.

| Peers behind                                   | Path                    |
|------------------------------------------------|-------------------------|
| ordinary NATs (one mapping per inside port)    | direct                  |
| the same NAT, on one LAN                       | direct, over the LAN    |
| symmetric NATs on both sides                   | through the server      |

In a TAP mesh network, frames to another client's own MAC go direct.
Broadcasts, and devices behind a bridging client, go through the server.

The server hands out the pair keys, so it could read what its clients send
each other. That is the same trust as in hub mode, where the server forwards
everything.

`camex --status` on the server shows:

- `RELAYED BY THE SERVER`: the pairs whose traffic the server forwards, with
  their rates;
- `PEERS (as the clients report them)`: each client's peers, path (`direct`,
  `relay`, `punching`) and rates. Clients report every 30 s, and the server
  does not see direct traffic itself.

`camex --top` puts both together: one `Mesh` line for each pair in its
network's block, with its path (`direct`, `relayed`, `punching`) and its rate
both ways: the clients' last report for a direct path, the server's own count
for a relayed one. The summary's `Relayed` is all the server relays.

`--json` has `relayed` and `peers` for the same.
