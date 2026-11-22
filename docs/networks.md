<!-- Copyright (c) OpenIPC  https://openipc.org  MIT License -->

# Networks

A client joins a network with three things: the server's address, the
network's name and its own ID. The ID is the MAC of the default route's
interface unless `--name` gives one. The server derives the client's address
from the network and the ID. The client gets the same address after a client
restart, a server restart or a move to another server with the same file.

```sh
camex --server-host vpn.example.org --network "field cameras of the north site"
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
- Logs, `camex --status` and `--top` on the server show the network by its
  name (cut to 31 bytes), or by `label=` when the block has one. They are
  root's, who can read the server's file anyway. A network the server only
  knows by its ID shows as `net-` plus the first 4 bytes of the ID in hex.
- The client hides `--network` from `ps` by overwriting it in its argument
  list.

Extra protection on top of the name, all optional:

| What                                    | Stops                                                                                                                |
|-----------------------------------------|----------------------------------------------------------------------------------------------------------------------|
| `--server-pubkey` on the client         | a fake server at the very first contact with a server that takes new networks (see below)                            |
| `psk=` in the client's `[client ID]`    | someone who knows the name posing as that client ID; the client then needs `--psk` with that key besides `--network` |
| `client_isolation = yes` in `[network]` | clients of the network reaching each other                                                                           |
| `allow_unknown_clients = no`            | client IDs without a `[client ID]` block                                                                             |

## The server's file

```ini
[server]
# Needed: the networks' gateways go on this tunnel
local_cidr = 10.200.0.1/24
# Any name makes a new network (off by default)
# allow_new_networks = yes
# Where derived pools come from, and their size (the defaults)
# network_supernet = 10.0.0.0/8
# network_prefix = 16

[network field cameras of the north site]
# Shown instead of the name in logs and status (optional)
label = cams

[network 7kq2m9x4p1lab8vz3t0c]
# This pool instead of a derived one
pool = 10.77.0.0/24
client_isolation = yes
```

`[network NAME]`: NAME runs from after `network` to the `]`, without the
spaces around it. It is 1 to 63 bytes and case-sensitive. A name shorter than
16 characters is refused unless its block has `weak_name = yes` (5.0.0). Such
a name could be guessed one CPace attempt at a time (see
[Guessing a name](#guessing-a-name)), and the client warns about it too.

| Key                | Meaning                                                                                                                                                                                         |
|--------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `pool`             | the network's IPv4 pool; without it a `/16` of `network_supernet` picked by the network's ID                                                                                                    |
| `client_isolation` | `yes` or `no`: drop packets between clients of this network; without it the server's setting                                                                                                    |
| `label`            | shown instead of the network's name in logs, `--status` and `--top`; up to 31 bytes without spaces                                                                                              |
| `selector`         | a public word (1 to 31 of `A-Z a-z 0-9 . _ -`): only a client with `--network-selector` that word gets CPace answers for this network (6.1.0, [below](#a-selector-one-network-per-attempt-610)) |
| `weak_name`        | `yes`: take a name shorter than 16 characters (a lab, a test); it could be guessed                                                                                                              |

`[server]`:

| Key                  | Meaning                                                                                                   |
|----------------------|-----------------------------------------------------------------------------------------------------------|
| `allow_new_networks` | `yes`: a client may name any network; the server makes it on its first handshake (see below)              |
| `network_supernet`   | the range derived pools come from, `10.0.0.0/8` by default                                                |
| `network_prefix`     | the size of a derived pool, 9 to 30, `16` by default                                                      |
| `cpace_per_ip`       | unconfirmed CPace attempts a minute from one address, `20` by default; `0` turns it off                   |
| `cpace_total`        | unconfirmed CPace attempts a minute from all addresses, `300` by default; `0` turns it off                |
| `cpace_ban_after`    | attempts over TCP in a row without a confirmed one that ban an address, `30` by default; `0` turns it off |
| `cpace_ban_time`     | seconds a ban lasts, `600` by default                                                                     |

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
know as a new network. It gets a derived pool, its gateway on the tunnel, and
the name a client sends, and it is listed with `(new)` in `camex --status`.
Since 6.0.0 a client creates a network only when told to: `--create-network`
(or a pinned key, `--server-pubkey`); without it, it refuses a server that
does not know the network (#199). A client sends the name only when it was given the server's key
(`--server-pubkey`) and the server says over CPace that it takes names: the
name then goes inside a handshake only that key opens. A client without a
pinned key never sends it, so a network only such clients use shows as
`net-`; the first pinned client that joins it later names it. Such
networks live until the server stops, or until a reload turns
`allow_new_networks` off. At most 256 networks exist at once, listed ones
included.

Anyone who knows the server's address can make networks then, so turn it on
only on a server meant to be open.

## The server's key

A client with `--network` and without `--psk` runs CPace at every
connection. Without `--server-pubkey` it checks the server by the network's
name:

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

With `--server-pubkey` the client skips KEYREQ and keeps nothing: the key is
the one it was given. CPace still runs, for its flags: `PAKE_FLAG_OPEN`, the
server takes new networks (the client then sends the ID, which only that key
opens), and `PAKE_FLAG_NAME`, the server takes the name in REGISTER. A
server that does not answer CPace gets the ID after three tries, and no
name: only that key can read it.

**Networks the server does not know yet** (`allow_new_networks = yes`) are the
exception. The server cannot know a name it never heard, so a new network is
still created the old way: the network's ID goes inside Noise message 1, and
the server's key is trusted on first use (kept without the mark). Since 6.0.0
a client does this only when told to, with `--create-network` (#199):
without it, a server that does not know the network but takes new ones is
refused (`it takes new ones, so --create-network would create it there`),
since a fake server would say just that to get the network's ID. Even with
it, the fallback happens only while the file has no `pake` mark for that
server: a server that once proved it knows the network and then says it does
not know it is refused as an impostor (`not trusted (an impostor?)`).

A server that does not answer `PAKE1` is not one to fall back to either:
every camex that knows `--network` answers CPace, so it is down or not the
real server. The client says so once (`does not answer CPace (down, or not a
camex 5 server?): trying again`) and keeps asking (#199; before 6.0.0 it took
the old way after three tries).

In short:

- listed networks are protected at every contact, the first one included,
  unless the client was given `--create-network`;
- creating a new network on an open server (`--create-network`) is protected
  only by trust on first use;
- `--server-pubkey` closes that gap too.

### Why trust on first use, and what it leaves open

A camera has no screen and no keyboard. Typing a 44-character key into each
one is where setups fail. `camex --server-host H --network NAME` is meant to be
the whole setup. Trust on first use (TOFU) makes that possible where CPace
cannot help. This is the SSH model: the first contact is believed, and every
later one is checked against it.

What each setup stops, at the first contact and after it:

| The client has                                                     | A fake server at the first contact                                                                                                                                        | Later contacts                               |
|--------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------|----------------------------------------------|
| `--server-pubkey`                                                  | fails: the key is not its own                                                                                                                                             | the same                                     |
| `--network`, a network the server lists                            | fails: it cannot answer CPace without the name; one online guess per connection. A fake server that ignores CPace or claims to take new networks is refused (6.0.0, #199) | the same (CPace at every connection)         |
| `--network --create-network`, a new network (`allow_new_networks`) | **believed**: the client joins the attacker's server, which gets the network's ID and can guess the name **offline**                                                      | pinned: a server with another key is refused |
| `--psk` without `--server-pubkey`                                  | cannot finish the handshake, but gets the first message to test **psk guesses offline**                                                                                   | pinned                                       |

The offline guess is the cost of the "believed" rows. The network's ID is a
BLAKE2b hash of its name. A server that holds it can test names as fast as it
can hash. A passphrase psk is stretched with Argon2id, so testing it is much
slower. A random psk (`hex:`, `base64:`) cannot be guessed at all.

TOFU is a reasonable default here, for these reasons:

- **Only the first contact is exposed, and only on the path.** The attacker
  has to sit between this client and the server at that very moment, before
  the key is kept. Later contacts are checked against the kept key.
- **A listed network does not use it.** The default `install.sh` setup lists
  one network with a random 20-character name, so its clients take the CPace
  path from the first contact on, and since 6.0.0 nothing on the path can
  turn them off it.
- **The name defeats offline guessing.** The name `install.sh` makes has 20
  lowercase letters and digits, about 103 bits, and cannot be guessed offline.
  A short or common name can.

When this is not good enough:

- **Pin the key.** Give `--server-pubkey` (`install.sh` prints it, and the
  connect link carries it), or ship `/etc/camex/known_servers` in the
  firmware. The key is public: a firmware dump gives nothing away, and
  networks are still made with nothing but `--network`. Only a client with
  `--server-pubkey` sends a new network's name, so that the server shows it
  by its name.
- **Prefer listed networks** to `allow_new_networks = yes`.
- **Use random names and random psks.** Never use a word or a short phrase.
- **Watch the first contact.** The client logs the key it kept
  (`First contact with server ...`). Check it against the server's
  `public_key` before trusting the client's traffic.

Proposals to narrow it further: [#199](https://github.com/OpenIPC/camex/issues/199).

## Guessing a name

A client that does not know the name can still try names. One `PAKE1` tests
one name against every network the server lists. The client checks the tags
of `PAKE2` alone, so a wrong guess shows on the server only as a `PAKE1` that
no `PAKE3` follows. Since 5.0.0 the server counts exactly those (`[server]`
keys above):

- **per address:** at most `cpace_per_ip` unconfirmed attempts a minute. A
  `PAKE3` that confirms gives its attempt back, so real clients spend nothing,
  however often they reconnect;
- **in all:** at most `cpace_total` unconfirmed attempts a minute from all
  addresses together, so many addresses cannot guess at many times the rate.
  An address whose client confirmed within the last hour is not held by this
  limit;
- **ban:** `cpace_ban_after` attempts in a row from one address with none
  confirmed refuse its CPace for `cpace_ban_time` seconds
  (`CPace: 30 attempts in a row from ... : its CPace refused for 600 s`). An
  address with a client that confirmed within the hour (a NAT with real
  cameras behind it) is limited, never banned. Over UDP a packet's source
  can be forged, so only attempts over TCP, whose handshake proves the
  address, count toward a ban; a ban then refuses the address over UDP and
  TCP alike. UDP attempts are held by the two limits above only. The
  addresses with a confirmed client are kept apart from the rest, so a
  flood of sources cannot push them out;
- **cookie** (6.0.0, #207): once more than half of `cpace_total` is spent,
  a UDP `PAKE1` from an address with no confirmed client gets a short
  `COOKIE` answer instead (37 bytes for 1200), a keyed hash of its address
  and port under a secret made anew every 2 minutes. Only a `PAKE1` that
  brings it back, in its padding, is counted and answered, so forged sources,
  which never see the answer, no longer use up the total. Clients before
  6.0.0 do not know `COOKIE`: under such a load they wait for it to pass.

`camex --status` shows the counts
(`cpace: 12 attempts, 10 confirmed, 2 refused, 0 bans (0 banned now)`), and
`--status --json` shows them under `cpace`.

With the defaults, one address tests at most 20 names a minute and all
addresses together at most 300. A random name of 20 characters
(`install.sh`) is beyond reach either way. These limits are what keeps a
short name from falling within days.

### A selector: one network per attempt (6.1.0)

Without more, a client gives the server nothing but the name, inside CPace,
so `PAKE2` has an answer for every network the server lists (up to 24): one
guessed name is tested against all of them at once. A server with many
networks can give each a **selector**, a public word like a user name:

```ini
[network 7kq2m9x4p1lab8vz3t0c]
# Public, like a user name: typed on the device
selector = north
```

```sh
camex --server-host cams.example.org --network 7kq2m9x4p1lab8vz3t0c --network-selector north
```

A `PAKE1` with a selector is answered for that one network (`camex --status`
shows each network's selector); a network with
a selector is answered only to a `PAKE1` that names it, so a guess tests one
network at a time. A `PAKE1` without a selector is answered for the networks
without one, as before. An unknown selector gets one answer of random bytes,
so the reply does not tell which selectors exist. The selector is 1 to 31 of
`A-Z a-z 0-9 . _ -`, one network to a selector, and must not come from the
name (anything derived from the name could be tested offline). It travels in
`PAKE1`'s padding: a server before 6.1.0 ignores it and answers for all its
networks, so a client with a selector still joins an older server
([#201](https://github.com/OpenIPC/camex/issues/201)).

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
camex 5.0.0 server   up 49s   UDP+TCP port 5800   tunnel 10.201.0.1/24   19:19:17
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

TAP networks, bridging a LAN and mesh networks are built in by default, in
both build profiles ([build.md](build.md#profiles-and-flags)): `camex-<arch>`
(full) and `camex-<arch>-client` have them. `CAMEX_L2=0` and `CAMEX_MESH=0`
leave them out:

```sh
make PROFILE=client CAMEX_L2=0 CAMEX_MESH=0   # a client with TUN networks only
```

A camex built without them refuses what it lacks, and says so:

- a server refuses `mode = tap` or `mesh = yes` in its file;
- a client refuses a server's `MODE=TAP`;
- a client built without mesh never asks for it, and its network's other
  clients reach it through the server.

## TAP networks (`mode = tap`)

```ini
[network the office lan, floor two]
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
  when it stops. An address the host already knows as a LAN device's (an
  entry it learned itself, or one somebody set) is never taken over by a
  remote device that claims it: the claim is ignored and logged. A stale
  entry is asked for by ARP first, and taken only if nobody answers.
- **Offloads:** the kernel says which frames still need their checksum
  (checksum offload) and which are TCP segments merged by TSO or GRO
  (`PACKET_VNET_HDR`). camex completes the checksums and cuts the merged
  segments into frames of the interface's MTU.
- **Loops:** every bridge sends a probe frame (EtherType `0x88B5`) on its LAN
  every 5 s. A bridge that sees another bridge of the same network there with
  a lower client ID stops bridging, and says so, until that one has been
  silent for 15 s.

`--bridge-mode` sets how frames go onto the LAN:

| Mode   | When                                                   | What passes                                       |
|--------|--------------------------------------------------------|---------------------------------------------------|
| `full` | Ethernet, a Wi-Fi access point or 4-address (WDS) link | every frame, as it is                             |
| `nat`  | a Wi-Fi client (station)                               | IPv4 and ARP, with the host's MAC towards the LAN |
| `auto` | the default                                            | `nat` on a Wi-Fi interface, `full` elsewhere      |

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
[network cameras that meet directly]
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

| Peers behind                                | Path                 |
|---------------------------------------------|----------------------|
| ordinary NATs (one mapping per inside port) | direct               |
| the same NAT, on one LAN                    | direct, over the LAN |
| symmetric NATs on both sides                | through the server   |

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
