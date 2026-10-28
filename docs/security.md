<!-- Copyright (c) OpenIPC  https://openipc.org  MIT License -->

# Security model

camex is a small tunnel for cameras, not a general-purpose VPN. Know what it
protects before exposing it to an untrusted network.

## Protocol v2 (camex 3.x)

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
  [Notes](usage.md#notes) for the exact rules.
- **Source addresses.** A client may send only from its own tunnel address;
  packets with another source address are dropped.
- **Replay protection.** Data packets have a 2048-packet window per session.
  While a client has a live session, a handshake with an older timestamp is
  rejected; a replayed handshake can never take a working session over, since
  a new session replaces the old one only with the client's first packet on it
  (a client whose clock went back after a reboot still reconnects). Packets of
  an ended session cannot be replayed into a new one: its keys are different.
  Such a handshake is logged once in 30 s for the client (#135):

  ```
  Dropped handshake from 203.0.113.7:41508 (cam1): its timestamp is 95 s older
  than the latest accepted for this client, live, last from 198.51.100.4:33279:
  a replay, another client with this ID, or this client's clock is behind (it is
  taken once the client is silent for 120 s)
  ```

  with `[N more since the last such line]` at its end when more came in the
  meantime. Not an attack, it is one of two things: another client runs with
  the same `--name` and key (the last address says where the other one is;
  stop one of them, or give it its own ID), or this client's clock is behind
  that of the one that made the live session (set the time, NTP). Either way
  this client gets in once the other has been silent for `client_timeout=`.
- **Roaming.** A session is found by its index, not by the client's address:
  after a NAT rebinding the client keeps its session.
- **Config reload.** After `SIGHUP`, a live session whose key was removed or
  changed, or now belongs to another client, ends.

## What it does not protect

- **The server's private key** lets its holder impersonate the server to
  clients that know the pre-shared key: keep `server_key` secret.
- **Metadata.** Packet sizes, timing, endpoints and the cleartext header (packet
  type, session index, counter) are visible.
- There is **no unencrypted mode**; for an open tunnel without encryption see
  OpenIPC VTesse.
- **Inside the tunnel** traffic is not filtered: any client can reach the server
  and other clients on the tunnel network. A firewall on the server's tunnel
  interface (`tun0` or similar with `/dev/net/tun`, `camex` with the kernel
  module; the server logs its name at startup) sees only traffic between the
  clients and the server's own network. A packet from one client to another is
  delivered inside camex and never reaches that interface, so no firewall rule
  stops it: set `client_isolation=yes` in `[server]` (or `--client-isolation`)
  to drop such packets (#83). It applies on reload; `camex --status` counts the
  dropped packets.

## Denial of service limits

- TCP connections that have not completed a handshake are limited (4 per
  source address, 32 in total) and closed after 10 s. A handshake carries a
  MAC keyed with the server's public key: packets from anyone who does not know
  it are dropped after one hash. The server processes at most 200 handshakes
  per second. Since every client knows that key, the handshake and key-trial
  budgets are shared out by source: unknown sources together get at most 3/4
  of each, one address at most 1/8 and one `/24` at most 1/4 (at least enough
  to try every key once). A live client rekeying from its address keeps the
  rest for up to 4 handshakes a second, and its own key is tried first (after
  the global key), so a flood from other addresses does not stop it. Cameras
  behind one NAT address therefore (re)connect at up to 25 a second, for example
  after a power cut, when they use the global key; with per-client keys a new
  handshake may try every key, so one address gets about max(51200/8, number of
  keys) key trials a second: with thousands of keys only a few clients a second.
  The others retry.
- Log messages that a peer can trigger (dropped packets, rejected
  registrations) are rate-limited; the number suppressed is logged.
- The auto pool hands out only client addresses of its network (by default
  `10.200.0.2`–`.254`) and never gives one address to two clients (#81):
  - when a client's session ends (it went silent for `client_timeout=`, 120 s
    by default; its TCP connection closed; its slot was needed), its address
    stays leased to its ID: no other client gets it while the pool has other
    free addresses, and never within 10 minutes plus `client_timeout=`; the
    client gets it back when it returns;
  - for 60 s after the server starts, clients get only the address they ask for
    (their previous one), so returning clients come back before new clients take
    addresses; a new client waits and retries;
  - a reload that gives a live client's address to a `[client ID]` `local_cidr`
    ends that client's session (it comes back with another address), and the
    profile's client is refused while another client still has the address;
  - a manual client cannot claim a live client's address (only the same manual
    client, with the same ID and key, can take its entry over);
  - this holds within one server: two servers (say a UDP and a TCP one) do not
    know each other's clients, so give each its own `auto_pool=` network.
- `allow_unknown_clients=no` serves only auto-mode IDs listed in the config file
  (manual clients are bound by their key and address, see [Notes](usage.md#notes)).

## Recommended deployment

- Keep the server key file readable by the server only (`umask 077`); give the
  clients `public_key` only.
- Prefer raw keys (`hex:`/`base64:`) or long random passphrases.
- Give each camera its own `psk=` in a `[client ID]` block, and set
  `allow_unknown_clients=no` when all auto-mode cameras are listed.
- Run the server with only the capabilities it needs (`CAP_NET_ADMIN`; with the
  kernel module also file access to `/dev/camex`, see below), firewall the
  tunnel interface, and set `client_isolation=yes` unless clients must reach each
  other.

## Kernel module permissions

`/dev/camex` is created `0600`, and `open()` requires `CAP_NET_ADMIN`; see
[Key Design Decisions](kernel-module.md#key-design-decisions). The module does no encryption:
encryption is done by the userspace app.

## Android app

The app uses `VpnService`, so Android shows the VPN key icon while the tunnel is
up; there is no way around that with `VpnService` (a client without TUN is
tracked in #21). Only the tunnel network and the routes the server sends go
through the tunnel; a `0.0.0.0/0` route from the server is ignored unless
**Route all traffic through the tunnel** is enabled in the settings. That
switch covers IPv4 only: IPv6 traffic does not go through the tunnel.
