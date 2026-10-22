# Changelog

The version lives in `CAMEX_VERSION` in `src/version.h`. From 3.0.2 on, binaries are
published only as the [`latest`](https://github.com/OpenIPC/camex/releases/tag/latest)
release, rebuilt from every push to `master`; this file is the change history.

## 3.1.1 — 2026-10-22

- Server: a live auto-mode client keeps its address until it confirms the new one
  (#77). A changed `[client ID]` `local_cidr` or `auto_pool=` used to switch the
  client's address at the handshake, before the client had the new CONFIG: packets
  still in flight from the old address were dropped, and a lost reply left the server
  and the client on different addresses until the next handshake. The new address is
  now pending and reserved until the client's first packet on the new session; a
  failed handshake or an unconfirmed session drops it and the old one stays.
  The previous session's address stays taken while that session still receives, and
  a manual handshake cannot claim another client's pending or previous address.
- Tests: the auto pool test covers a lost reply after a pool change (fails before), the
  previous session's address and a manual handshake for a pending address.

## 3.1.0 — 2026-10-21

- Server: the auto pool is configurable with `auto_pool=<network>/<prefix>` in
  `[server]`, from `/8` to `/30` (#75). The first address is the clients' gateway,
  clients get the second to the last but one with the pool's prefix. Without the key
  the pool stays `10.200.0.0/24`. A reload applies a new pool; auto-mode clients move
  to it with their next handshake. The server warns when its tunnel address is not the
  pool's gateway or the pool is outside its tunnel network (before, such a server
  silently handed out addresses it had no route to).
- Server: the number of client slots is configurable with `max_clients=` (1–65534, read
  at startup); without it the number of pool addresses, at least 256, so a `/24` pool
  keeps today's 256 (#75). Slots, `[client ID]` profiles and keys are allocated at
  runtime instead of fixed arrays of 256; over TCP the slots stay below `FD_SETSIZE`.
  Slots, profiles and indexes that do not fit the memory lock limit are allocated
  unlocked, with a `SECURITY WARNING` that names the limit to raise. TCP slots also stay within the
  open files limit, and `accept()` failures such as `EMFILE` no longer spin the
  server: new connections wait a second. A reload that asks for another number of slots says a restart is
  needed.
- Server: a handshake tries the per-client keys in turn, now at most 51200 tries a
  second for all handshakes (the old worst case, 200 handshakes × 256 keys) or twice
  the number of keys when there are more, so that every key stays reachable; the
  global key is always tried first and not counted.
- Server: finding a client no longer scans all slots (#75). The session index of a
  packet carries the client's slot number, and clients, profiles and keys are found
  through indexes by address, ID and key; a reload no longer compares every session
  with every key.
- Tests: the auto pool test covers a `/16` pool with 600 clients and traffic to a client
  above slot 256, `max_clients=`, a `/30` pool, invalid values, a pool change by reload,
  the TCP limit, the pool warning and slots above the memory lock limit.

## 3.0.2 — 2026-10-17

- Linux release binaries (`arm`, `mipsel`, `aarch64`, `x86_64`) are static and linked
  against musl, and no longer packed with UPX (#54, PR #56). The 3.0.1 ones were linked
  dynamically against glibc 2.38 and did not start on OpenIPC firmware (musl or uClibc,
  no glibc). Toolchains: OpenIPC (arm), OpenWrt soft-float (mipsel, runs without an
  FPU), Bootlin musl (aarch64, x86_64); `ci/build-musl.sh` builds them and runs the unit
  tests under qemu-user. Checked on OpenIPC ARM and MIPS cameras.
- Android: the APK is 517 KB instead of 2.5 MB (#55). Release resources are shrunk and
  only the `en`, `ru` and `zh-rCN` translations of the libraries are kept (PR #57);
  AppCompat and Material are gone, the app uses platform classes and themes (PR #58).
  The settings are a separate screen instead of a bottom sheet, with the same fields
  and checks; "Show the key" replaces the password toggle.
- Releases: one release, `latest`, with stable file names and `SHA256SUMS` (#60); the
  versioned releases `v2.3.8`, `v3.0.0` and `v3.0.1` are replaced by this file (#59).

## 3.0.1 — 2026-10-16

- A changed auto-mode CONFIG (address, gateway, MTU, routes) is applied to the live
  tunnel without restarting the client (#49, PR #50). Failed routes are retried on the
  next handshake; a `CAMEX_TUN_FD` tunnel is kept and the log asks for a restart; on
  macOS the new utun gets the new address and MTU.
- Client: a stop request while connecting (for example a slow TCP connect) is seen within
  a second (PR #53).
- Tests: the #23 gaps are closed (#12 end to end, #15 expiry and reuse, #10 one-way
  loss); the Android device test runs its server in its own Docker network (PR #50).

## 3.0.0 — 2026-10-15

**Breaking: protocol v2 replaces v1** (#17, PR #48). A 3.x client or server does not
talk to 2.x, and there is no unencrypted mode. Update the server and all clients
together.

- Protocol v2: a `Noise_NKpsk0_25519_ChaChaPoly_BLAKE2b` handshake (PR #46), rekeying
  under traffic, replay protection. The server has a key pair (`--genkey`); clients
  need the server public key and the pre-shared key. Keys are raw 32-byte values or
  Argon2id passphrases (#14, PR #47). Closes #13 and #14.
- Server: client keys are bound to client IDs and spoofed tunnel source addresses are
  dropped (#5, #6, PR #29); REGISTER is replay-checked and a session is bound to the
  client's nonce prefix (#7, PR #35); TCP connections hold a limited pre-auth slot until
  they register (#9, PR #37); the auto pool hands out unique .2–.254 addresses, and
  `allow_unknown_clients` controls clients without a config block (#15, PR #38).
- Hardening (#16, PR #39): rate-limited peer-triggered logs, keys revoked on reload, an
  `FD_SETSIZE` guard, a TCP connect timeout. CONFIG addresses are validated, and macOS
  runs `/sbin/route` without a shell (#12, PR #28).
- Networking: per-connection TCP send buffers, pending data dropped on close (#8,
  PR #25); TCP frames no longer desync on partial reads under bursts (PR #4); the
  client detects a lost server and recovers after a server restart (#10, PR #26).
- Kernel module: `/dev/camex` is restricted to `CAP_NET_ADMIN`, and the module builds
  against current kernels (#11, PR #27).
- Android: the VPN interface is created from the server's CONFIG and the app's own
  traffic stays outside it (#18, #19, PR #30); the APK of every new release is posted to
  the OpenIPC Telegram channel (#43, PR #44).
- Internal: a packet backend interface instead of direct TUN calls (#20, PR #41).
- Docs: README in Russian and Simplified Chinese (PR #32), the security model (#22,
  PR #40), Android screenshots (#42, PR #45).

## 2.3.8 — 2026-06-19

- Android: OpenIPC launcher icon and more connection settings (PR #3).
- CI: secrets are no longer used in `if:` conditions.

## 2.3.7 — 2026-06-16

- Android: the APK can be signed and runs; connection settings in a bottom sheet (PR #2).
- One version source (`src/version.h`) for all targets; `make apk` handles a signed APK.

## 2.3.6 — 2026-06-15

- Fixes from a code review (11 items); out-of-tree builds with `BUILD_DIR`.

## 2.3.5 — 2026-06-15

- Windows builds for x86_64 and i686, checked under Wine in CI.

## 2.3.4 — 2026-06-15

- Keepalive packets and a configurable server timeout.

## 2.3.3 — 2026-06-15

- Automatic addresses for clients without a config block; REGISTER is rate-limited.

## 2.3.2 — 2026-06-15

- `EINTR` handling, less log noise from rate limiting, `accept()` retried, PID file
  removed on exit.

## 2.3.1 — 2026-06-15

- Fixes: a TCP buffer overflow, data loss with several clients, the send nonce prefix
  rotated on reconnect, the encrypted DATA sequence number on the server.
- MinGW builds fixed; release asset names normalized.

## 2.3.0 — 2026-06-14

- MinGW build warnings fixed.

## 2.2.0 — 2026-06-14

- Sources moved to `src/`; WinTUN adapter for Windows and release packaging; UDP NAT
  handling and TCP registration fixes; macOS, Windows and Android CI builds.

## Before 2.2.0

- 2026-04-28 onwards: the initial camex code — userspace TUN tunnel over UDP and TCP
  with a server, clients and the optional `camex.ko` kernel module.
