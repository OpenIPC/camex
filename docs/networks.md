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

- The client never sends the name. It sends the network's ID, BLAKE2b-256 of
  the name, inside Noise message 1. Only the server's private key opens that
  message, so someone watching the traffic sees neither the name nor the ID.
- The server never logs the name. Logs and `camex --status` show `label=`
  or `net-` plus the first 4 bytes of the ID in hex.
- The client hides `--network` from `ps` by overwriting it in its argument
  list.

Extra protection on top of the name, all optional:

| What                                    | Stops                                                                      |
|-----------------------------------------|----------------------------------------------------------------------------|
| `--server-pubkey` on the client         | a fake server on the very first contact                                    |
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

A client without `--server-pubkey` asks the server for its public key on
first contact (a `KEYREQ`, 64 bytes, answered with 37). It trusts that key
from then on and keeps it in the known servers file:

- `/etc/camex/known_servers` by default; `--known-servers <path>` sets
  another path, `--known-servers none` keeps nothing;
- one `<host>:<port> <base64 key>` line per server, rewritten through a new
  file so a power cut leaves either the old or the new one.

The kept key is used from then on, and never replaced on its own. When a server
gets a new key, its clients' handshakes fail; after two silent rounds the
client says `The server's key came from <file>: if the server got a new key,
remove its line there (or give --server-pubkey)`.

Without a writable file, the key is asked for again at every start. This
works, but every start is then a first contact. On OpenIPC, `/etc` is on the
overlay and keeps the file.

## Status

`camex --status` lists the networks in its header, each with its label, pool
and number of clients. `camex --status --json` adds `networks` (`label`,
`pool`, `listed`, `clients`, `client_isolation`), `allow_new_networks`, and
for each client `network` and its traffic rates `rx_rate` and `tx_rate` in
bytes a second.

`camex --top` shows the clients redrawn every second (`--interval <s>` sets
the interval), busiest first, with RX/s and TX/s. Press `q` to quit.
