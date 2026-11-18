<!-- Copyright (c) OpenIPC  https://openipc.org  MIT License -->

# Changelog

The version lives in `CAMEX_VERSION` in `src/version.h`. From 3.0.2 on, binaries are
published only as the [`latest`](https://github.com/OpenIPC/camex/releases/tag/latest)
release, rebuilt from every push to `master`; this file is the change history.

## 4.2.0 — 2026-11-19

- Android library, settings (owner's decisions 2026-11-18, the interface review):
  - short labels: `Client name`, `Proto` (was Transport), `MTU (576–9000)`,
    `Keepalive, sec (0–3600)`, `Server timeout, sec (5–3600)`, `Web port for nginx (1–65535)`;
    what camex takes for an empty field is the field's new `hint` (`empty: the server's`,
    `empty: 10`, `empty: 20`, `empty: none`);
  - the client name of a new setting: `CamexTunnel.defaultName(context, app)`, the app's name and
    the last 4 characters of the device's ANDROID_ID (`camex-3f2a`, `vectis-3f2a`), and
    `CamexTunnel.fields(context, app)` with it as the name's initial value;
  - the pre-shared key is shown hidden, with `Field.SHOW_SECRET` ("Show the key") under it, the
    same in every app.

## 4.1.2 — 2026-11-18

- Android library, the gate (#187, review of #189): an acceptance is no longer kept. 4.1.1 kept the
  answer for 2 seconds by port alone, so a connection to another local address on the port, or one
  right after another app took the port over, could pass on an earlier check. Now every connection
  that passes is checked for its own tuple; only a refusal is kept, for its address and port.

## 4.1.1 — 2026-11-18

- Android library, the gate of 4.1.0 (#187, review of #188):
  - a minified app (R8) kept no `CamexVpnService.ownsListener()`, which camex finds by name, and
    refused even its own page: `consumer-rules.pro` keeps it;
  - TCP in IP fragments is dropped: a SYN split into fragments went past the gate, and the kernel
    put it together for another app's listener;
  - Android 5–9: only the listeners that could take the connection count (the tunnel address or
    any address, `::` and `::ffff:` included), and a `/proc/net/tcp{,6}` table that cannot be read
    refuses instead of being taken as empty;
  - the answer is kept for 2 seconds (the check ran on the packet thread for every SYN) and the
    refusals are logged at most once a minute, with their count.

## 4.1.0 — 2026-11-18

- Android library: a new TCP connection from the tunnel reaches only the app's own web page
  (#187). Its destination must be `web_port` and the socket listening there the app's (Android
  10+: `ConnectivityManager.getConnectionOwnerUid()`, earlier: `/proc/net/tcp`); any other gets a
  TCP reset back through the tunnel. Before, with several apps embedding the library on one
  device (one VPN at a time), one app's tunnel delivered connections to another app's server on
  the same port: the server published another app's page under this client's name. camex on
  Linux, Windows and macOS is unchanged (`src/tcpgate.c`, a gate only the library's JNI sets).

## 4.0.2 — 2026-11-18

- Windows build without warnings: the log line of a bad TCP frame length printed its sizes with
  `%zu`, which the Windows build's format check rejects, and could log wrong numbers there; the
  sizes are now printed as `unsigned long`. `tun_persistent`, used only outside Windows, is no longer
  defined in the Windows build. Found by `docker/test-windows.sh` (#184).

## 4.0.1 — 2026-11-17

- TCP transport: a frame whose body arrived in pieces lost its first piece (#180). The bytes read
  before an EAGAIN went into the caller's buffer, a local array of the event loop, and the next call
  went on in another one; the frame came out corrupted, the peer dropped it ("not a v2 packet") and
  the tunnelled TCP stalled. Measured on a 60 ms link: server to client crawled at ~10 KB/s (512 KB
  not within 30 s, 20 MB not within 3 minutes). The part of a body read so far is now kept per
  connection until the frame is complete. Both sides had it: the client's 4 MB socket buffers hid it
  on the server's side of the stream, and without them client to server stalled the same way.
- A server's TCP connections get the same 4 MB send and receive buffers as a client's socket
  (`net_tcp_tune_socket()`) once the client has registered; a pre-auth connection keeps the small
  default, so peers that never register cannot take more memory.
- Measured after the fix on a server with a 60 ms round trip, 20 MB each way: TCP server to client
  2.1–2.9 MB/s (was ~10 KB/s), client to server 2.3–2.4 MB/s; UDP unchanged (~2 MB/s and 8–11 MB/s).
- Tests: `docker/test-e2e-throughput.sh` (in CI): 4 MB through the tunnel both ways over UDP and TCP
  with a 30 ms netem delay on each side, all of it and at least 300 KB/s each, within one deadline
  per transfer; it found 0 bytes server to client over TCP before the fix. A unit test of a body that arrives in two pieces into two buffers.

## 4.0.0 — 2026-11-17

- **A server now listens on UDP and TCP at once** (#178): `--transport both`, the new default,
  takes clients over UDP and over TCP on the same port, with one client table and one address
  pool, so a camera keeps its address whichever transport it comes over. A client that comes
  back over the other one with the same ID and key moves its entry: over TCP at once, over UDP
  on its first packet on the session of that UDP handshake (not of a rekey over TCP, and not
  before the handshake's reply went out); the server closes its old TCP connection and logs the
  move. A move that fails leaves the client where it was, and a move starts its connection age
  (`connected_s`, the start page's Connected) again. `--transport udp` or `--transport tcp` keeps a server on one.
- **Upgrade note**: a server started without `--transport` (`install.sh` writes none for UDP)
  was UDP only and now listens on TCP on its port as well; open TCP in the firewall if clients
  are to use it. If either port cannot be had, a server in `both` does not start and says which
  one and what to do (another server on that port: run one server in `both`, or give it
  `--transport udp` or `tcp`); its UDP socket is bound without `SO_REUSEADDR`, so a UDP server
  already on the port is found too instead of the two splitting the clients' packets. On Windows
  the TCP listener of any server no longer sets `SO_REUSEADDR` either: there it let a second
  socket take a port another one listens on. Two servers that served UDP and TCP on one port with separate
  pools become one in `both`.
- Clients do not change: UDP unless set to TCP (`--transport tcp`, `t=tcp` in the connect link,
  the switch in the Android app), never moving to TCP by themselves; `--transport both` is
  refused for a client. The connect link and the command `install.sh` prints stay UDP.
- `camex --status`: the header says `udp+tcp`, a new `VIA` column the transport of each client;
  `--json` has `"transport":"both"` for the server (each client's `transport` stays `udp` or
  `tcp`). The control socket of a server in `both` is `camex-both-<port>.sock`.
- The start page has a **Transport** column, UDP or TCP per device (a dash from an older
  server): the page asks the new `CLIENTS INDEX VIA`, whose records start with `L:udp` or
  `L:tcp` (an older server: `CLIENTS INDEX`, unchanged for older readers), and a change of transport wakes
  `camex-nginx-sync --follow`.
- A server in `both` keeps all its client slots: only TCP connections are held to what
  `select()` can watch (`FD_SETSIZE` and the open files limit; fixed at startup, as the slot
  table is), UDP clients take no descriptor. A quarter of those connections, at most 32, stays
  for pre-auth ones, so a client whose old connection is still open can come back on a new one;
  a new client over the limit is refused at its handshake, and at the limit the oldest idle
  pre-auth connection makes room. A server reads at most 4096 datagrams a pass, so a
  steady stream of UDP does not hold up TCP clients, the tunnel or the control socket.
- `install.sh --transport udp|tcp|both` (`both` by default, nothing written to `CAMEX_ARGS`);
  it says to open UDP and TCP for a server in `both`.
- Tests: `docker/test-e2e-both.sh` (a UDP and a TCP client at once on one pool, the transport
  in the status and on the start page, a client that switches transport keeps its address, also
  from a live TCP entry cut off without its connection closing, one ID over both at once, a taken
  TCP port and a UDP port held by another server, `both` refused for a client, the TCP limit
  with a small open files limit); unit tests of
  the start page's transport column. The status and nginx-map suites keep testing a UDP server
  (`--transport udp`); the install suite checks the new default.
  Docs: `docs/server.md`, `docs/usage.md`, `docs/install.md`, `docs/nginx.md`,
  `docs/security.md`, `docs/android.md`, `docs/android-library.md`, the READMEs.

## 3.19.0 — 2026-11-16

- A client's own web port for nginx (#174): `web_port=` in `[defaults]` or a `[client ID]`
  block, and `--web-port <port>` for an auto client, which announces it in its handshake (only
  when given: a server before 3.19.0 refuses a client that sends it). The order: the client's
  block, the announced port, `[defaults]`, `--upstream-port`. `camex --status --format
  nginx-map` asks the new `CLIENTS WEB` (an older server: `CLIENTS`) and writes
  `<address>:<port>` per client; `--status` shows the port after the address, `--json` as
  `web_port`; a changed port wakes `camex-nginx-sync --follow`. The Android library takes it as
  `web_port` (`CamexTunnel.WEB_PORT`), and `server_timeout` too.
- The Android app is built on the tunnel library (#172): one JNI, one VPN service, one link
  parser in the repository. The app now stays inside its own VPN (camex's sockets protected),
  reconnects by itself until Stop (when camex exits: after 1 s, doubling to 60 s; camex itself
  reconnects to its server with its own back-off) and shows the state (connecting, connected with the address,
  VPN permission needed, why it did not start). The **Route all traffic through the tunnel**
  setting is gone: camex on Android is for reaching the cameras, not a VPN for all traffic;
  `r` in a link is read and ignored. The foreground service type is now `specialUse`.
- The Android library (and so the app) refuses server routes wider than a `/8`, as it refused
  `0.0.0.0/0`: two halves, `0.0.0.0/1` and `128.0.0.0/1`, would take all traffic too (#175).
- `tests/android/tunnel-e2e.sh` on Android 5.0 waits up to 60 s for the other client after
  `stop()` and `start()` in one process and reports `KNOWN (#173)` when Android routes the app
  outside its new VPN (half of the runs: reached in 4 s, or not in 3 minutes); still open.

## 3.18.0 — 2026-11-16

- Android library `android/tunnel` (#170): camex as a VPN client inside another app (Decoder,
  microsnander), taken through a submodule of this repository. The app stays inside its own
  VPN (camex's sockets are protected from it), only the tunnel subnet and the server's routes
  go through it, never a default route; the app's listening sockets are reached through the
  tunnel. A static API without Activity or stored settings: `fields()`, `check()`,
  `consentIntent()`, `start()`/`stop()`, the state (`STOPPED`, `CONNECTING`, `CONNECTED`,
  `NEEDS_CONSENT`, `FAILED`) with the address and the last error, a listener; reconnects
  until stopped; the notification can be the app's own; `ConnectLink` reads camex links into
  the values. Unit tests in CI, `tests/android/tunnel-e2e.sh` on the emulator
  ([docs/android-library.md](docs/android-library.md)).
- camex resets getopt at the start of each run, so a second `camex_main()` in one process
  (the Android app or library restarting camex in-process) reads its options again; it used
  to read none and exit with `The client needs a key: --psk`.
- nginx (`examples/nginx-camex.conf`, installed by `install.sh --nginx`): `client_max_body_size 0`
  in the clients' server block, so an upload through a device's web UI (a camera's firmware, a
  flash image written through microsnander's page) is no longer stopped at nginx's 1 MB (413).
  `install.sh` keeps an existing file as before and now warns when it has no
  `client_max_body_size`.
- Android: the app and the library take the camex sources from one list,
  `android/camex-sources.cmake`.

## 3.17.2 — 2026-11-15

- Android: `-PcamexAbis` must be a non-empty list of known ABIs (arm64-v8a, armeabi-v7a, x86_64,
  x86), otherwise the build stops with that list (#168); an empty entry (`arm64-v8a,,x86_64`) is
  an error too. An empty value used to build every ABI of the NDK, x86 and x86_64 too; the
  default `arm64-v8a,armeabi-v7a` is unchanged.

## 3.17.1 — 2026-11-14

- The device page (`camex --status --format nginx-index`, #166): the tab icon lamp has a
  thin brass rim (`#6e5320`, radius 6.5), the same icon as the other brass pages; its
  colours are unchanged.

## 3.17.0 — 2026-11-09

- The device page (`camex --status --format nginx-index`, #164): the browser tab has an
  icon, a lamp like those of the rows: green when every device is online, red when one is
  offline, grey while the page gets no answer. It works without JavaScript and follows the
  in-place refresh. Offline devices are listed first in each list, then the online ones,
  each group by name and ID as before; an unchanged state still gives the same file.

## 3.16.1 — 2026-11-09

- The device page (`camex --status --format nginx-index`, #162): the browser tab shows the
  state, `3 online, 1 offline · Camex Devices` (the offline part only when there are offline
  devices), also without JavaScript and after an in-place refresh; while the page gets no
  answer it starts with `No connection ·`. Secondary text is easier
  to read: `--dim` is `#b09873`, the footer `#a08a69`, rows of offline devices are dimmed
  less (`opacity` .95); the offline count on the tabs is `#e8755c` on an inactive tab and
  `#4a0e08` on the active one. They reach 4.5:1 against their background except at the far
  ends of two gradients: the footer on a very tall window (about 4.4:1 at 1900 px) and the
  active tab's count at the bottom edge of the brass (about 4.4:1).

## 3.16.0 — 2026-11-02

- The Android app runs on Android 5.0 and later (`minSdk` 21, was 26; #158): older TV boxes
  and phones. Before Android 8 the service starts without `startForegroundService` and the
  notification has no channel; the link's key check decodes base64 itself; before Android 6
  the fields a link changed are marked through their background. A launcher icon for
  Android 5–7 (the brand kit's fallback, a rounded square), the adaptive one from Android 8.
- `tests/android/device-test.sh` runs on Android 5–7 too: `install -g` only from Android 6,
  the MTU from `ip addr` where SELinux hides `/sys/class/net` (Android 7), taps retried while
  a window animates in. PASS on emulators with Android 5.0, 6.0 and 7.1.

## 3.15.2 — 2026-11-02

- Android app (#156): the OpenIPC brand icon, the same in all OpenIPC Android apps. Launcher
  icon: the OpenIPC logo in white on `#4C60D8` (was a PNG on `#4D60D9`), a vector with a
  monochrome layer for themed icons; the name comes from the launcher label. Android TV: the
  app is on the home screen now, with a banner (the icon, then "Camex" on a white panel),
  `LEANBACK_LAUNCHER`; leanback, touchscreen and Wi-Fi are not required (Ethernet-only boxes).
  The resources are made by the OpenIPC brand kit.

## 3.15.1 — 2026-10-28

- `install.sh` on a rerun (#149): an older unit's `--port` or `--psk` in `CAMEX_ARGS` of
  `/etc/default/camex` wins over `camex.conf`, so the options are compared with it and the
  installer says to edit it there.
- The Android link (#154): none when the server's public key is not known to the installer
  (a key file of its own without `public_key=`, or `server_key=` in `camex.conf`); the page
  says when a link has no port or transport, so the desktop command uses camex's defaults.
- e2e: both in `test-e2e-install.sh`, the page's note in jsdom. `docs/install.md`.

## 3.15.0 — 2026-10-28

- A link that opens the Android app with a server's settings (#146). `install.sh` prints it
  next to the client's command line:
  `https://openipc.github.io/camex/connect/#h=<host>&p=<port>&t=<udp|tcp>&k=<server public key>&s=<psk>`
  (optional `n` client name, `m` MTU, `a` keepalive, `r` route all traffic). The settings are
  after `#`: the browser keeps them, they reach no server. The link has the psk: it is a
  password.
  - The app opens it (and `camex://connect?…`) on its settings screen: the link's values
    filled in, the changed fields marked, malformed ones named and left out. Nothing is saved
    or connected until Save.
  - `docs/connect/index.html` on GitHub Pages: when the link opens in a browser, the server's
    settings (not the psk), "Open in Camex", the APK, and the desktop client's command. It
    loads nothing and sends no referrer.
  - An App Link: `https://openipc.github.io/.well-known/assetlinks.json` (the content of
    `android/assetlinks.json`, OpenIPC/openipc.github.io#26) names the release key, so the
    link opens the app directly; an APK signed with another key gets the page.
- `install.sh --public-host <name|address>`: the host in the printed command and link (else
  the domain, else this host's public address, else a placeholder).
- Tests: the link parser on the JVM (`gradle testReleaseUnitTest` in CI), the page in jsdom
  (`docker/test-connect-page.sh`), the printed link decoded in `test-e2e-install.sh`.
  `docs/android.md`, `docs/install.md`.

## 3.14.3 — 2026-10-28

- A handshake dropped because its timestamp is not newer than the latest accepted for a
  live client (#135) is logged with why: how many seconds older it is, the client's last
  address, and the likely causes (a replay, another client with the same ID and key, or
  this client's clock behind); once in 30 s per client, with the number not logged since.
  The handshake is still dropped. `docs/security.md`.
- e2e `test-e2e-register-replay.sh`: the new text, one line for several within 30 s.

## 3.14.2 — 2026-10-28

- `install.sh --domain` in another script (#151): `мойдомен.рф` (any case) is taken in its
  IDNA form `xn--d1acklchcc.xn--p1ai`, made by `idn2` (installed for it), and said once;
  nginx, `domain=` of `camex.conf` and the printed addresses use that form, as DNS does.
- e2e: such a domain on a host with another one (its IDNA form said, refused), one with a
  blank refused as given. `docs/install.md`.

## 3.14.0 — 2026-10-28

- The start page refreshes itself in place every 5 seconds (#142): its inline script fetches
  the same file again and swaps the counts and both lists, keeping the chosen tab and the
  scroll, so a device that goes off turns red without a reload. A failed fetch (the server
  down, a new password) keeps the data and shows "no connection"; a page of another camex
  version is loaded once in full. One request at a time: no answer in 4 seconds is "no
  connection", an answer to an older request is dropped, and the same page as before changes
  nothing (a selection or the focus stays). Over HTTPS a peer's link keeps its own port.
  Without JavaScript it reloads every 30 seconds as before (`<meta refresh>` inside
  `<noscript>`). No other file, library or CDN.
- Tests: the page's version and `<noscript>` (unit); the script run in jsdom (`node:24-slim`)
  against the page the sync wrote: lists swapped in place, the tab kept, "no connection" and
  back, a reload for another version, one request at a time with a 4-second limit, the same
  page rebuilding nothing, a late answer dropped, a peer's link kept over HTTPS (e2e).
  `docs/nginx.md`.

## 3.13.0 — 2026-10-28

- `client_timeout=` in `[server]` (#143): how long a silent client stays before its session
  ends, 60 to 120 s (default 120, as before); a reload applies it, and the minimum time an
  address stays leased to a client that is gone follows it (10 minutes plus it).
  `install.sh` writes `client_timeout=60` into a new `camex.conf` (`--client-timeout <s>`), so
  a device that goes off turns red on the start page after about a minute instead of two.
- Clients send a keepalive at least every 20 s (a third of the lowest `client_timeout=`), also
  with `--keepalive 0` (every 20 s, was 60) or a larger `--keepalive`, so they stay alive
  against any server. A client before 3.13.0 with `--keepalive 0` or above 20 s needs a server
  with `client_timeout=120`.
- `camex --status --wait`: the WAIT commands to servers that do not take them share the
  wait's deadline (#144).
- Tests: the key's range (unit); a silent client dropped after 61 s with `client_timeout=60`,
  one with `--keepalive 3600` kept (e2e); `install.sh` writes and takes it. Docs:
  `docs/server.md`, `docs/security.md`, `docs/nginx.md`, `docs/usage.md`, `docs/install.md`.

## 3.12.0 — 2026-10-28

- `install.sh` takes more options (#141), settings made once like the files they go to: on
  a rerun a different value is not applied, and the installer says which line to edit
  where.
  - `--psk <key>`: the clients' pre-shared key (`hex:`, `base64:` or a passphrase) instead of
    a random one.
  - `--transport udp|tcp`: the server's transport; the printed client command has it.
  - `--pool <network>/<prefix>` (`/8` to `/30`): the clients' pool, the server at its first
    address.
  - With `--domain`: `--password <password>` for the start page instead of a random one;
    `--http-port <port>` for nginx (its `listen` lines, the start page's links, what
    `--check` asks, the addresses printed); `--upstream-port <port>` for the cameras' web
    port.
- `camex --status --format nginx-index --http-port <port>`: nginx's port in the start page's
  links (none for 80). `camex-nginx-sync` takes `HTTP_PORT` from its service for the links
  and `--check`.
- e2e: every option on a new host (a TCP server on its port and pool, the given psk and
  password, nginx on 85, the cameras on 8080), a rerun with other values that changes none
  of them; a unit test of the links with a port. `docs/install.md`, `docs/nginx.md`,
  `docs/usage.md`.

## 3.11.2 — 2026-10-27

- `camex --status --wait` notices a server that starts while it waits (#138): it looks at the
  control sockets' directory every second, also when there was no server or only sockets
  left behind, and a new socket ends the wait as a change. `camex-nginx-sync --follow` so
  maps a new server's clients within about a second instead of at the end of its interval
  (10 s with `INDEX`, 300 s without). Not with `--control` (one server only).
- The `WAIT` command is written until it is whole, by the wait's deadline; a server that
  does not take it in time counts as unchanged, not as gone.
- e2e: a server started during a 30-second wait ends it in about 2 s, and the next token has
  its state. `docs/nginx.md`: a server that starts or stops is a change.

## 3.11.1 — 2026-10-27

- `camex.ko` refuses to load when it was built for another kernel configuration (#134):
  `alloc_netdev()` runs in the kernel and sets `num_tx_queues`, `real_num_tx_queues` and
  `gso_max_size`; read through the module's own offsets into `struct net_device` they must
  be 1, 1 and the kernel's default (65536). Otherwise `insmod` fails with `ENOEXEC` and a
  message in `dmesg` that names the cause and `docs/kernel-module.md`, instead of an oops
  in `register_netdevice` (#133, `CONFIG_WIRELESS_EXT` on in the camera's kernel, off in the
  SDK's) or an interface that drops every packet (#73).
- The kernel module VM test builds a second `camex.ko` against headers with two pointers
  added before `netdev_ops` (as `WIRELESS_EXT` adds them) and checks that it refuses to
  load without an oops; then the real one loads as before. Checked on 4.15, 6.1 and 6.8.
  `docs/kernel-module.md` describes the check.

## 3.11.0 — 2026-10-27

- The nginx map follows the servers' changes instead of a 10-second timer (#130):
  - `camex --status --wait <token> [--wait-timeout <s>]` waits until the clients of any
    server on the host (or of `--control`) change — a client comes, goes, gets another
    address or peer, a reload brings other names or another domain — then prints the
    token of the new state; `-` prints it at once. The servers answer a new control
    command, `WAIT <hash> <seconds>`, with a fingerprint of that state, worked out at most
    every 250 ms while someone waits; the counters (last packet, traffic) are not in it.
    The control socket takes 8 connections at a time (was 4).
  - `camex-nginx-sync --follow [<seconds>]`: a sync at every change, about a second after
    it, and none while nothing changes; with `INDEX` (the start page's live columns) also
    every 10 seconds, without it every 300. With a server before 3.11.0 it syncs every 10
    seconds and says so once.
  - `camex-nginx-sync.service` runs `--follow` (`Type=simple`, `Restart=always`, enabled
    for `multi-user.target`); `camex-nginx-sync.timer` is gone. `install.sh` stops and
    removes the timer of 3.10 and restarts the service on every run; `--check` checks the
    service.
- e2e: `docker/test-e2e-nginx-map.sh` checks `--wait` and `--follow` (a new client in the
  map within 2 s, no sync while idle, the fallback for an older server);
  `docker/test-e2e-install.sh` the service after a restart of camex and the removal of the
  timer. Docs: `docs/nginx.md`, `docs/install.md`, `docs/usage.md`.

## 3.10.0 — 2026-10-27

- `install.sh`: the camex server on Debian or Ubuntu in one command, run straight from
  GitHub (#129): `curl -fsSL https://raw.githubusercontent.com/OpenIPC/camex/master/install.sh
  | sudo sh`. Its steps are listed at the top of the script and printed before it starts,
  then `[n/8]` at each one:
  - camex of the `latest` release for the host's architecture in `/usr/local/bin`, the
    server's key pair, `/etc/camex/camex.conf` (0600: port, pool, client isolation, a new
    `psk=`), `/etc/default/camex` and the service; it prints the clients' public key, psk
    and command line.
  - `--domain <domain>` also installs nginx in front of the clients: nginx's file, the
    sync script with its service and timer, the start page's password; `domain=` is added
    to `camex.conf`, then `camex-nginx-sync --check` runs. A rerun without `--domain` keeps
    nginx's part up to date; another domain is refused.
  - A rerun updates camex, the scripts and the units; the settings made once (`camex.conf`,
    `server.key`, `/etc/default/camex`, nginx's file, the password) stay as they are.
  - `--uninstall` lists every file of camex, which are there, and the commands to remove
    them; it removes nothing itself.
  - Other systems are refused with a message: install by hand.
- `examples/camex-nginx-setup.sh` is removed: `install.sh --domain` replaces it. Its setup
  moves to the new names on the first run: `/etc/nginx/conf.d/camex.conf` becomes
  `camex-nginx.conf` with its edits, `/usr/local/bin/camex-nginx-sync.sh` becomes
  `camex-nginx-sync` (the old one kept in `/var/backups/camex/`).
- `examples/camex.service`: the arguments in `/etc/default/camex` (`EnvironmentFile=`), so
  a new unit never loses them (the arguments of an older unit move there on the first run
  of `install.sh`); `RuntimeDirectory=camex` for the control socket of `camex --status`
  under `ProtectSystem=strict`; `systemctl reload camex` sends SIGHUP.
- `examples/camex-nginx-sync.service` runs `/usr/local/bin/camex-nginx-sync`.
- A new e2e test (`docker/test-e2e-install.sh`, run by CI on the release binary): Debian 12
  with systemd, a client connecting with what the installer printed, nginx, reruns, the
  older layout, `--uninstall`.
- Docs: the README (en/ru/zh-CN) keeps what camex is for, the installation, the Android and
  web screenshots, the release links and a list of the docs; the rest moved to `docs/`
  (English): `install.md` (with the files on the server and a clean reinstall),
  `usage.md`, `server.md` (with the keys of `camex.conf`), `nginx.md` (with the sync
  script), `security.md`, `android.md`, `kernel-module.md`, `build.md`, `releases.md`.
  Every file has the OpenIPC header (the kernel module stays GPL-2.0). The scripts and
  configs of `examples/` and `install.sh` keep only that header,
  where the file is installed and a link to its docs.

## 3.9.2 — 2026-10-26

- Android: the launcher icon's logo fits the adaptive icon again (#123). The foreground drew
  the OpenIPC logo and wordmark out to 42 dp from the centre, beyond the 72 dp the launcher
  shows and the 66 dp circle its mask keeps, so round and squircle masks cut off the "O" and
  the "C". It is scaled to 71% (within 30 dp) at every density; the background and the
  pre-Android 8 icon are unchanged.

## 3.9.1 — 2026-10-26

- A client started before the network is ready waits for it instead of exiting (#121):
  - `--auto` without `--name` and no default route: it waits for one (every 5 s) to take
    the ID from its interface's MAC; it used to exit with "Client name must be set with -n".
  - Waiting for its config there is no 300 s limit any more ("Timeout: server did not
    provide config"). Without an answer for `--server-timeout`, or when the handshake
    cannot be sent, it makes a new socket and resolves the name again, so a new address or
    route is picked up; a new address used to end it with "Initialization failed".
  - The ID taken from the MAC is kept in `/tmp/camex-id` once the server has answered, and
    a restart (a crash, the service, the app) takes it from there until a reboot, whatever
    the network looks like then. `--name` still wins; the file is used only if it is a
    regular file of camex's own user that no one else can write. Not on Windows.
- `--keepalive 0` no longer turns off loss detection: a keepalive still goes every 60 s,
  and a lost server is noticed after 60 s + `--server-timeout`, then the usual reconnect.
- A new e2e test (`docker/test-e2e-client-start.sh`, run by CI); README (en/ru/zh-CN),
  `--help`.

## 3.9.0 — 2026-10-26

- nginx start page: a **Peer** column right after Address (#119): the external IP each
  camera connects from, as the server sees it (after NAT), linked to
  `http://<IP>:<port>/` in a new tab (no port in the link for 80). A camera that is not
  connected shows `—`; on a phone the column is hidden with Address. The page's panel is
  wider (1200px) for it. `camex --status`, `--json` and the map do not change.
- `peer_port=` in `[defaults]` or a `[client ID]` block: the port of that link, the
  client's own value first, then `[defaults]`, then 8080; `none` shows the IP without a
  link. 1 to 65535 or `none`, anything else stops the load; in `[server]` it is an error.
  The server puts the port in its start page records (`CLIENTS INDEX` gets two fields
  before the name), so a reload applies it; with an older server the column shows `—`.
- The start page always asks for a password (#119): `examples/nginx-camex.conf` turns on
  `auth_basic` (user `camex`, `/etc/nginx/camex.htpasswd`).
  - `camex-nginx-setup.sh` generates the password with `openssl`, keeps it in
    `/etc/camex/nginx-password` (0600) and prints it at the end of every run; a rerun keeps
    it, `--new-password` makes a new one. Other users in the htpasswd file stay.
  - In an nginx file it keeps (edited, or from an older version) it sets only the access
    lines of the start page block, so a rerun or an update turns the password on.
  - `camex-nginx-sync.sh --check` warns when the start page has no password, and opens it
    with the one in `/etc/camex/nginx-password` (`PASSWORD_FILE=`).
- Docs: `docs/nginx.md`, README (en/ru/zh-CN), `examples/README.md` and
  `examples/camex.conf`; new screenshots of the start page.

## 3.8.3 — 2026-10-25

- The version is taken from `src/version.h` by every build (#117):
  - Android: `android/app/build.gradle.kts` reads it at build time (`versionName` X.Y.Z,
    `versionCode` X*10000 + Y*100 + Z). Before, the APK said 3.5.0 because the values were
    written out by hand. The manifest has no version attributes any more, and
    `scripts/sync-android-version.sh` is gone.
  - The kernel module: `src/Kbuild` no longer puts `git describe` (a commit hash, as there
    are no tags) in place of the version.
  - `make apk` names its file after it.
- CI checks that the APK's version matches `version.h`; the kernel module VM test checks
  `camex.ko`'s version.

## 3.8.2 — 2026-10-25

- Android: the app is "Camex", no longer "Camex VPN" (#115): the label under the launcher
  icon, the VPN session name in Android's VPN dialog and settings, the notification and
  its channel. The main screen's buttons are "Start" and "Stop". The manifest and the
  service take the name from `app_name`.
- `tests/android/device-test.sh` taps the buttons by their exact text, so "Stop" does not
  match "camex stopped".

## 3.8.1 — 2026-10-25

- `camex --help`: `--domain` names its default (`domain=` in `[server]` of the server),
  and `--format nginx-index` describes the page's tabs and values (#113).
- Docs (#113):
  - README (en/ru/zh-CN): `domain=` and `name=` under Server Configuration; the NAME
    column of `--status` and `name` of `--json`; the map's `# domain:` line; the
    domain's default; `CHECK_DNS`.
  - `docs/nginx.md`: the map's `# domain:` line, the names in `--status`, and that
    only camex 3.8.0 servers report `domain=` (with an older one, give `--domain`).
  - `examples/README.md`: an nginx part with `camex-nginx-setup.sh` and `--check`.

## 3.8.0 — 2026-10-25

- One domain, given once (#106): `domain=` in `[server]` of the server's file.
  `camex --status --format nginx-map` and `nginx-index` take it from the running servers
  (a new `DOMAIN` control command; a server older than 3.8.0 counts as none). `--domain`
  overrides it, with a warning when they differ; servers with different domains and no
  domain at all are warned about too. A `domain=` that is no host name stops the load.
  The map names its domain in a `# domain:` comment.
- `camex-nginx-sync.sh`: `DOMAIN` is optional (default: the server's `domain=`), and
  `camex-nginx-sync.service` no longer sets it. After each new map the script warns when
  there is no domain, or when nginx has no `server_name` for it or does not include the
  map. `--check` goes through the whole setup (servers, domain, nginx, start page, DNS,
  timer), one `ok` / `WARN` / `FAIL` line per item, exit code 1 on a `FAIL`.
- `examples/camex-nginx-setup.sh --domain <domain>`: installs the nginx server blocks,
  the sync script, its service and timer and the start page's directory with that domain,
  sets `domain=` in `camex.conf` and reloads the camex servers, checks and reloads nginx,
  enables the timer and runs `--check`. Safe to run again; an edited file is kept unless
  `--force` (the old one in `/var/backups/camex/`); `--dry-run`.
- Tests: unit tests of the map's domain line; the nginx e2e test checks the domain from
  `domain=`, `--domain` overriding it, no domain after a reload, the warning and `--check`
  for a domain nginx does not serve, and the setup script on a clean host (install, run
  again, an edited file, `--force`, another domain).

## 3.7.0 — 2026-10-25

- nginx start page (#110): two tabs, **Configured** (every `[client ID]` block of the
  server's file, connected or not; the offline ones with a red lamp and no link, counted
  on the tab) and **Auto** (the connected clients from the auto pool), switched with CSS
  and remembered by the browser. Each row adds, as in `camex --status`: how long the
  client has been connected, its last packet, its last handshake and their number, and
  RX / TX in KiB (thousands grouped). On a phone fewer columns are shown. Sorted by name
  in both tabs, connected or not.
- Control socket: a new `CLIENTS INDEX` command (one tab-separated record per live
  client with its numbers, and per configured client that is not connected); `CLIENTS`
  and `CLIENTS NAMES` are unchanged. `--format nginx-index` needs a 3.7.0 server.
- `camex-nginx-sync.sh`: the page changes at every run now (ages, traffic); the log says
  so only when the number of devices online changed.
- Tests: unit tests of both tabs, offline rows, the values and malformed records; the
  nginx e2e test checks an offline configured client, the tabs, the handshakes against
  `--json`, and that the traffic on the page grows with pings.

## 3.6.0 — 2026-10-25

- nginx start page (#105): `camex --status --format nginx-index [--domain …]` prints a
  static HTML page of the live clients, one row each with its name, ID and tunnel
  address, the name linked to `//<client ID>.<domain>/`, in the look of OpenIPC/vectis's
  page (brass panel, a lamp per device; the address column hidden on a phone). Sorted
  by name, then ID, with no time in it, so an unchanged state gives an identical file.
  IDs that are not a host name label and duplicates are handled as in the map.
- Server: an optional `name=` in a `[client ID]` block, UTF-8 text up to 64 bytes
  without control characters, display only (the host name still uses the ID). A file
  with a name that is not valid, or with `name=` in `[defaults]`, is refused. `camex
  --status` shows it in a NAME column (when a client has one), `--json` as `"name"`.
- Control socket: a new `CLIENTS NAMES` command (`CLIENTS` is unchanged, so
  `nginx-map` of 3.5.x keeps working against this server).
- Examples: `camex-nginx-sync.sh` writes the page with `INDEX=` when it changed (no
  nginx reload); `camex-nginx-sync.service` sets it with `StateDirectory=camex-nginx`;
  `nginx-camex.conf` serves it on the bare domain, with commented `allow`/`deny` and
  `auth_basic` lines. `docs/nginx.md`: "Start page", with screenshots (also in the README).
- Tests: unit tests of the page and of names; the nginx e2e test checks the page on the
  bare domain, its links, the escaped name, `--status`/`--json`, and that an unchanged
  state leaves the file alone.

## 3.5.2 — 2026-10-23

- Linux: IPv6 is turned off on the tunnel interface (`disable_ipv6`) before it comes
  up, so it no longer sends router solicitations or MLD reports into the tunnel, where
  the server dropped them (#103). An interface that outlives the client (`camex.ko`)
  gets its previous setting back on exit; a setting that cannot be changed is logged.
- `/dev/net/tun` and `/dev/camex` are opened with `O_CLOEXEC`: a child process cannot
  keep `camex.ko` (one open at a time) busy.
- Without either device the error says what to do: "load tun.ko or camex.ko".
- Tests: `docker/test-kmod-vm.sh` with `CAMEX_BINARY=<static camex>` also runs the
  client in the QEMU guest over the real `camex.ko` against a server in the container:
  address and route from the server, IPv6 off, ping both ways, the interface left down
  without address and route and with its IPv6 setting back after SIGTERM. In CI as "Kernel module VM test"; the release waits for it.

## 3.5.1 — 2026-10-23

- Client with `camex.ko` (`--tun-dev /dev/camex`, or any device but `/dev/net/tun`):
  the interface belongs to the module and stays after the client, so on exit the
  client now brings it down and removes its address; with them go the tunnel
  network's route and any route through the interface (#99). Before, `camex` kept
  `UP`, the address and the route until `rmmod`. A device passed in from outside
  (a TUN provider, `CAMEX_TUN_FD`) is left as it is.
- Tests: `test-e2e-persistent-tun.sh` (a persistent tun interface standing in for
  `camex.ko`: clean after SIGTERM, reused by the next client; `/dev/net/tun` unchanged).

## 3.5.0 — 2026-10-23

- `camex --status --format nginx-map [--domain …] [--upstream-port …] [--default …]`
  prints an nginx `map` of the live clients: `<client ID>.<domain>` → `<tunnel
  address>:<port>`, sorted, lower case; IDs that are not host name labels are left
  out with a warning, host keys are quoted (an ID that is one of map's own words —
  `default`, `include`, `hostnames`, `volatile` — gets map's `\` as well), `--default`
  is `<IPv4>:<port>`, digits only, and written out normalised; no output and exit code 1 when a running
  server does not answer, a socket left by a server that is gone is skipped (#84).
  The server answers a new control command, `CLIENTS`.
- `examples/camex-nginx-sync.sh` writes the map when it changed, checks and reloads
  nginx and puts the old map back if nginx rejects it, one run at a time; a map counts
  as applied only after a reload, so a failed reload (nginx not running yet) is tried
  again; an interrupted run puts the old map back; runs take turns through `flock`, so a
  slow run keeps its turn and a killed one leaves no lock behind; `camex-nginx-sync.service` and
  `.timer` run it every 10 seconds. `examples/nginx-camex.conf`: a server block that
  opens each client's web UI by name, with WebSocket, no buffering and an offline page.
- `--format table|json` as another way to write `--json`.
- Tests: `test_nginxmap` (the formatter) and `test-e2e-nginx-map.sh` (nginx in front of
  two clients: pages by name, the offline page, no reload without a change, an
  invalid ID, a client that goes away, a run waiting for another, IDs that are map
  keywords).

## 3.4.0 — 2026-10-22

- Server: `client_isolation=yes` in `[server]` (or `--client-isolation`) drops packets
  from one client to another (#83). Such a packet is delivered inside camex and never
  reaches the server's tunnel interface, so a firewall there could not stop it; any
  client with the key could reach every other one. Traffic between the clients and
  the server is unchanged. The default stays `no`; a reload applies a change.
  `camex --status` shows whether it is on and counts the dropped packets, in total
  and per client. The README now says which traffic the tunnel interface sees.
- Tests: `test-e2e-isolation.sh` checks both settings, a reload each way and the
  command-line option.

## 3.3.0 — 2026-10-22

- Server: `camex --status` lists the connected clients of every server on the host
  (#82): ID, mode, key (global or own), tunnel address (and a pending one), peer
  address, transport, connected and last-packet times, the last key renewal, traffic
  in both directions, and the addresses kept for clients that are gone (#81).
  `--status --json` prints the same as JSON for scripts and monitoring; `--control
  <path>` asks one server. The server answers on a local control socket,
  `/run/camex/camex-<transport>-<port>.sock` by default (with the address or interface
  of `--bind-ip` / `--bind-dev` in the name when set), mode `0600` (only its user,
  normally root); `control_socket=` in `[server]` or `--control` changes it, `none`
  turns it off, a reload applies a change. Answers end with a marker line, so a cut-off
  answer is an error; each server has 5 s to answer. Not on Windows.
- Tests: `test-e2e-status.sh` checks the table, the JSON, the counters, the socket
  permissions, a UDP and a TCP server on one host, `control_socket=none` and a reload
  that moves the socket (or keeps it when the new path cannot be had).

## 3.2.1 — 2026-10-22

- Server: the auto pool never gives one address to two clients (#81). Reported: two
  auto clients with different IDs had the same address. Reproduced on 3.1.1 in four
  ways, all fixed:
  - a client that went silent (`--keepalive 0`, an outage) expired on the server while
    it still had its address, and the address went to the next client. Now the address
    of an auto client whose session ends (expiry, a closed TCP connection, its slot
    taken) stays leased to its ID: other clients get it only when the pool has no
    other free address, and never within 12 minutes; the client gets it back;
  - after a server restart, a new client could take the address of a client that had
    not come back yet. For 60 s after the start, clients now get only the address they
    ask for (their previous one); new clients wait and retry;
  - a reload that gave a live client's address to a `[client ID]` `local_cidr` let the
    profile's client register with it too. The reload now ends that client's session
    (it comes back with another address), and the profile's client is refused while
    another client has the address;
  - a manual client with the global key took over a live auto client's entry and
    address. It is now refused; only the same manual client (same ID, same key) takes
    its entry over.
- Client: a TCP client whose connection the server closes before its first CONFIG
  (no address yet: the start hold, a full pool) reconnects and retries within its
  wait instead of exiting.
- This holds within one server: two servers (a UDP and a TCP one, say) do not know
  each other's clients and need different `auto_pool=` networks (README).
- Tests: `test-e2e-multi-client.sh` runs real auto clients through restarts, expiry,
  a full pool, a reload, a manual claim and slot eviction (9 checks fail on 3.1.1).

## 3.2.0 — 2026-10-22

- Server: the handshake budgets are shared out by source (#78). Every client knows the
  server's public key, so anyone with it could send handshakes that no key opens and
  use up the 200 handshakes and the key trials a second for everybody; with a large
  keystore two such handshakes were enough. Unknown sources together now get at most
  3/4 of each budget, one address at most 1/8 and one `/24` at most 1/4 (but always
  enough to try every key once). A live client rekeying from its address keeps the
  rest for up to 4 handshakes a second, and its own key is tried right after the
  global key, so its rekeys cost one key trial; more handshakes from its address, or
  other keys, count as from an unknown source. Sources are placed in the per-second
  table by a keyed hash. The key
  trial budget is four times the keystore when that is above 51200 (was twice).
  Cameras behind one NAT address now (re)connect at up to 25 a second with the global
  key; with per-client keys one address gets about max(51200/8, number of keys) key
  trials a second, so with thousands of keys only a few a second. The others retry.
- Tests: `test-e2e-fair-budget.sh` floods bogus handshakes from one address and from
  eight `/24`s; other clients register and a live client keeps rekeying (fails before).

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
  The log says `Offered … (it moves there when it confirms the new session)` instead
  of `Auto-assigned …` for such an address, then `Client … moved to …` or `Client …
  did not confirm …: it keeps its address`.
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
