# Deployment with nginx: many cameras, one server, each web UI by name

This guide sets up one Linux host that runs the camex server and nginx side by
side, for many cameras (or other devices) behind NAT. Each camera connects with
camex in auto mode, gets a tunnel address from the server's pool, and its web UI
opens through nginx by name:

```
http://<camera ID>.openipc.click/   →   http://<its tunnel address>:80/
```

Nobody keeps a list of addresses: nginx follows the server's live state through
`camex --status --format nginx-map` and a small sync script. The bare domain,
`http://openipc.click/`, opens a start page with every connected camera, its
name and a link (see [Start page](#start-page)).

## Topology

```
 cameras (behind NAT)                    server host (Linux)
 ┌─────────────────────┐   UDP 5800    ┌─────────────────────────────────────┐
 │ camex --auto        │ ────────────▶ │ camex --mode server                 │
 │   --name <MAC>      │               │   tunnel interface camex/tun0       │
 │ web UI on :80 of    │ ◀── tunnel ── │   pool 10.200.0.0/22                │
 │ its tunnel address  │               │                                     │
 └─────────────────────┘               │ nginx :80/:443 *.openipc.click    │
                                       │   map from camex --status           │
 users ── http(s)://<id>.cam.… ──────▶ │   proxy_pass → <tunnel address>:80  │
                                       └─────────────────────────────────────┘
```

- **camex server**: authenticates the cameras, gives each a stable tunnel
  address (a returning camera gets its address back, #81) and carries their
  traffic. Its control socket answers `camex --status`.
- **nginx**: the only way users reach the cameras. It maps the host name to a
  camera's tunnel address and proxies to it; a name with no live camera gets an
  "offline" page.
- **camex-nginx-sync.sh**: rewrites nginx's map when cameras come or go and
  reloads nginx, and keeps the start page current.

Both run on the same host on purpose: nginx reaches the tunnel addresses
through the server's own tunnel interface, and `camex --status` needs the
server's local control socket.

## Server

Install camex (see the [README](../README.md#permanent-links)), create the key
pair once and give `public_key=` to the cameras:

```sh
sudo mkdir -p /etc/camex
sudo sh -c 'umask 077; camex --genkey > /etc/camex/server.key'
```

`/etc/camex/camex.conf`:

```ini
[server]
# One /22 serves up to 1021 cameras; the server is the first address
local_cidr=10.200.0.1/22
auto_pool=10.200.0.0/22
# No [client ID] list: every camera with the key gets an address
allow_unknown_clients=yes
# Cameras must not reach each other (see Firewall)
client_isolation=yes
# The cameras' domain, for the map and the start page (see nginx)
domain=openipc.click
# One pre-shared key for all devices (see the next section)
psk=hex:<64 hex digits>
```

Start it (or use `examples/camex.service`):

```sh
sudo camex --mode server --port 5800 --config /etc/camex/camex.conf \
    --server-key-file /etc/camex/server.key
```

- Size the pool for the fleet with room to spare: an address stays kept for a
  camera that is gone for at least 12 minutes before another one may get it.
- Addresses are stable: a camera that reconnects, or comes back after a server
  restart, gets its previous address, and the server never gives one address to
  two cameras (#81). The nginx map therefore changes only when cameras come and
  go.
- For 60 seconds after a start the server gives cameras only the address they
  ask for (their previous one), so returning cameras come back first; a new
  camera waits and retries.

## One pre-shared key for all devices

One key for the whole fleet is simple to deploy, and it is acceptable **only on
a network you trust**, such as a company LAN where the cameras and their
firmware are under your control. Know what it allows:

- any device with the key can connect and get an address;
- a device with the key can register under **another camera's ID**: it takes
  over that ID's session and address (also while the camera is online, as a
  reconnect would), so the camera's name in the nginx map leads users to the
  impostor;
- the key is on every camera, so one stolen camera exposes it.

Per-client keys change that: give each camera its own `psk=` in a `[client ID]`
block. A key is then bound to its ID, a device cannot register under another
ID, and `allow_unknown_clients=no` refuses IDs not in the file. The price is a
list to keep: one block per camera.

```ini
[client 001a2b3c4d5e]
name=Warehouse, entrance
psk=hex:<this camera's 64 hex digits>
```

`name=` is optional, with one shared key too: a block with only `name=` gives
the camera a name on the start page. Any `[client ID]` block lists the ID in
the file, though: with `allow_unknown_clients=no` it also lets that camera in,
with the shared key when the block has no `psk=` of its own.

## Cameras

Each camera runs camex in auto mode with its MAC address as the ID, which is
unique and stable:

```sh
camex --mode client --auto --name 001a2b3c4d5e \
    --server-host vpn.example.com --port 5800 \
    --server-pubkey <public_key> --psk hex:<64 hex digits> \
    --tun-dev /dev/camex
```

- **ID**: lower-case hex without separators, so it is a valid host name label
  (`001a2b3c4d5e.openipc.click`). IDs with `:` or `_` are left out of the map
  with a warning.
- **Kernel module**: on OpenIPC cameras without `tun.ko` load `camex.ko` and use
  `--tun-dev /dev/camex` (see [Kernel Module](../README.md#kernel-module)).
- **NAT**: nothing to forward on the camera's side; the camera connects out.
  The default keepalive (10 s) keeps the NAT mapping open and lets the camera
  notice a lost server within 20 s (`--server-timeout`).

## Firewall

The server's tunnel interface (`camex` with the kernel module, `tun0` or
similar with `/dev/net/tun`; the server logs its name at startup) carries the
traffic between the cameras and the server host. It does **not** carry traffic
from one camera to another: camex delivers that inside the process, so no
firewall rule on the interface can stop it. `client_isolation=yes` drops it
(#83); `camex --status` counts the dropped packets.

An `nftables` example for `camex`: cameras may only answer the server, the
server (nginx) may only open HTTP and ping on them, nothing is forwarded:

```nft
table inet camex {
    chain input {
        type filter hook input priority 0; policy accept;
        # From the cameras to this host: replies only
        iifname "camex" ct state established,related accept
        iifname "camex" drop
    }
    chain output {
        type filter hook output priority 0; policy accept;
        # From this host (nginx) to the cameras: HTTP and ping only
        oifname "camex" ct state established,related accept
        oifname "camex" tcp dport 80 accept
        oifname "camex" icmp type echo-request accept
        oifname "camex" drop
    }
    chain forward {
        type filter hook forward priority 0; policy accept;
        # Nothing routed into or out of the tunnel
        iifname "camex" drop
        oifname "camex" drop
    }
}
```

Load it with `nft -f /etc/nftables.d/camex.nft` (or include it from
`/etc/nftables.conf`). The camex protocol itself (UDP 5800 on the public
interface) is not affected. Keep `net.ipv4.ip_forward=0` unless the host must
route other traffic.

## nginx

The domain is given once. Put it in `[server]` of the server's file:

```ini
[server]
domain=openipc.click
```

The map and the start page take it from the running server (`camex --status`
asks for it), and the sync script needs no setting of its own. `--domain` (or
`DOMAIN=` for the script) still overrides it, with a warning when the two
differ. A reload (`kill -HUP`) applies a new domain. Only camex 3.8.0 or later
reports `domain=`. An older server reports none, so without `--domain` its
clients get the domain of the others, with a warning, or bare IDs when no
server reports one. With an older server, give the domain explicitly
(`--domain`, or `DOMAIN=` for the script): the map and the start page work as
before. The nginx configuration
must name the same domain; the setup script below writes it there for you.

**The quick way**: `examples/camex-nginx-setup.sh` does steps 3 to 5 below in
one go, with the domain filled in everywhere, then checks the result:

```sh
sudo examples/camex-nginx-setup.sh --domain openipc.click
```

It installs the nginx server blocks as `/etc/nginx/conf.d/camex.conf`, the sync
script with its service and timer, the start page's directory and its password
(see [Start page](#start-page), **Password**). It sets
`domain=` in `/etc/camex/camex.conf` (`--config` for another file) and reloads
the running camex servers, then checks and reloads nginx and enables the
timer. Run it again at any time: unchanged files stay, and a file edited since
is kept and its difference shown (`--force` replaces it and keeps the old one
in `/var/backups/camex/`, out of nginx's directories; `camex.conf` as
`camex.conf.bak`). `--index none` leaves out the start page, `--new-password`
makes a new password for it, `--dry-run` only says what it would do. It needs
`openssl` for the password. DNS and TLS (steps 1 and 2) stay yours.

**The checks**: `camex-nginx-sync.sh --check` goes through the whole setup and
prints one line per item, `ok`, `WARN` or `FAIL`. It checks that the servers
answer, the domain, that nginx includes the map and has `server_name` for the
domain, and what nginx really answers on port 80 of this host: a name under
the domain with no camera must get camex's offline page, and the bare domain
the start page, behind its password (a `WARN` when it has none; with the
password from `/etc/camex/nginx-password`, `PASSWORD_FILE=` for another file,
it also opens the page). Then the start page file, the DNS records and the
timer. It
exits with 1 on a `FAIL`. A DNS name that does not resolve is a `FAIL`; one
that resolves to an address that is not this host's is a `WARN`, because
behind NAT that is normal. `CHECK_DNS=no` skips the DNS items, e.g. in a
container. After each new map the sync script also logs a warning when there
is no domain, or when nginx has no `server_name` for it or does not include
the map. camex's own warnings (no domain, a bad ID) are logged when they
change, not at every run.

The steps by hand:

1. **DNS**: a wildcard record `*.openipc.click` pointing to the server host,
   and for the start page a record for `openipc.click` itself: a wildcard
   does not cover the bare domain.
2. **TLS** (optional): a certificate for both `*.openipc.click` and
   `openipc.click` (a wildcard certificate does not cover the bare domain
   either); uncomment the `listen 443 ssl` lines in both server blocks of the
   example.
3. **Server block**: copy `examples/nginx-camex.conf` to `/etc/nginx/conf.d/`
   and replace `openipc.click` with your domain (the one in `domain=`). It includes the map, proxies
   to the camera with WebSocket support and without buffering, gives up on a
   camera after 5 s and shows an "offline" page for names with no live camera.
4. **The map**: create an empty one so that nginx starts, then let the sync
   script keep it current:

   ```sh
   printf 'map $host $camex_upstream {\n}\n' | sudo tee /etc/nginx/camex-map.conf
   sudo cp examples/camex-nginx-sync.sh /usr/local/bin/
   sudo cp examples/camex-nginx-sync.service examples/camex-nginx-sync.timer \
       /etc/systemd/system/
   # The domain comes from domain= in [server] of camex.conf
   sudo systemctl daemon-reload
   sudo systemctl enable --now camex-nginx-sync.timer
   ```

   The script writes `camex --status --format nginx-map` to the map only when
   it changed, runs `nginx -t` and reloads nginx, and puts the previous map back
   if nginx rejects the new one; runs never overlap (it needs `flock`, from
   util-linux or busybox). When a running server does
   not answer it leaves the map as it is (a socket left behind by a server that
   is gone is skipped).

5. **The start page** (optional): `INDEX=` in the service (set in the
   example) makes the script write it, and `StateDirectory=` creates its
   directory, `/var/lib/camex-nginx`. The second server block of the example
   serves it on the bare domain, behind a password (user `camex`):

   ```sh
   printf 'camex:%s\n' "$(openssl passwd -apr1)" | sudo tee /etc/nginx/camex.htpasswd
   ```

   See [Start page](#start-page).

The map looks like this:

```
# camex --status --format nginx-map: generated, do not edit
# domain: openipc.click
map $host $camex_upstream {
    "001a2b3c4d5e.openipc.click" 10.200.0.2:80;
    "001a2b3c4d5f.openipc.click" 10.200.0.3:80;
}
```

## Start page

`camex --status --format nginx-index --domain openipc.click` prints a static
HTML page of the cameras in two lists, switched with the tabs at the top:

- **Configured**: every `[client ID]` block in the server's file, connected or
  not. A camera that is not connected has a red lamp and no link, so a missing
  one shows at once; the tab counts them ("Configured 5 · 2 offline").
- **Auto**: the connected cameras with no `[client ID]` block, which got their
  address from the auto pool.

Each row has the camera's name linked to `//<camera ID>.openipc.click/` (no
scheme, so the link works over HTTP and HTTPS alike), its ID and tunnel
address, its **Peer**: the external IP it connects from, as the server sees it
(after NAT), linked to `http://<IP>:<peer_port>/` in a new tab; and as in
`camex --status`: **Connected** (how long it has been on
this connection), **Last packet**, **Handshake** (how long ago, and how many
it has had) and **RX / TX** in KiB. The page opens Configured when there are
blocks in the file, Auto otherwise, and the browser remembers the tab chosen
(the tabs work without JavaScript too).

| Desktop                                    | Phone (fewer columns)                                       |
|--------------------------------------------|-------------------------------------------------------------|
| ![Start page](images/nginx-start-page.png) | ![Start page on a phone](images/nginx-start-page-phone.png) |

- **Peer links**: `peer_port=` in `[defaults]` or in a camera's `[client ID]`
  block sets the port of the link; the camera's own value wins, then
  `[defaults]`, then 8080. With port 80 the link has no port. `peer_port=none`
  shows the IP as plain text: in `[defaults]` for every camera (a camera with
  its own number still gets a link), in a `[client ID]` block for that one. A
  reload (`kill -HUP`) applies it. Only the page has the column: `camex
  --status` keeps its PEER column with the port, the map does not change. A
  camera that is not connected shows `—`, and so does every camera when the
  server is older than 3.9.0. On a phone the column is hidden with Address.

- **Names** come from the server's file: `name=` in the camera's `[client ID]`
  block, UTF-8 text up to 64 bytes without control characters. A camera without
  one is listed by its ID. A reload (`kill -HUP`) applies new names; the server
  refuses to load a file with a name that is not valid. `camex --status` shows
  the names in a NAME column, `--json` as `"name"`.
- **Order**: by name (A–Z in any case alike), then by ID, connected or not, so
  a camera keeps its place in the list.
- **Freshness**: the ages and the traffic are those of the last sync (every 10
  seconds with the example timer), and the page reloads itself every 30
  seconds. The script replaces the page at every sync and logs only when the
  number of cameras online changed.
- **The same rules as the map**: IDs that are not a host name label are left
  out with a warning, of an ID on two servers the connected one (then the
  lower address) is kept, and when a running server does not answer nothing
  is printed and the old page stays.
- **DNS and TLS**: the bare domain needs its own DNS record and must be in the
  certificate next to the wildcard (see [nginx](#nginx), steps 1 and 2).
- **With the map**: the script builds the page before the map, so every device
  on it is in the map too, and publishes it only after that map was applied.
  While nginx rejects a new map or does not reload, the page stays as it was,
  so its links never lead to names that nginx does not know yet.
- **Turning it off**: empty `INDEX=` stops the updates only; the script does
  not know which file it wrote before. Remove the start page's server block
  from `nginx-camex.conf` (or the file, e.g. `/var/lib/camex-nginx/index.html`)
  and reload nginx.
- **Password**: the page lists every camera on the server and where it
  connects from, so it always asks for a password (`auth_basic`, user
  `camex`). `camex-nginx-setup.sh` generates the password once, keeps it in
  `/etc/camex/nginx-password` (root only), writes
  `/etc/nginx/camex.htpasswd` (other users added there stay) and prints it at
  the end of every run. A run keeps the password, `--new-password` makes a
  new one. In an nginx file it keeps (edited, or from an older version), it
  sets only the access lines of the start page block, so every rerun and
  update turns the password back on. The example's block also has commented
  `allow` / `deny` lines, to let in only some networks besides.
- The sync script needs a server of camex 3.7.0 or later for the page: an older
  one answers the page's control command (`CLIENTS INDEX`) with an error, and
  the page is not written (the map still is).

## Checks

- **Who is connected**: `sudo camex --status` lists each camera with its ID,
  address, peer, last packet and traffic (and its `name=` in a NAME column),
  and the addresses kept for cameras that are gone; `--json` gives the same
  for scripts and monitoring.
- **A camera directly**: `curl -sI http://10.200.0.2/` on the server host. No
  answer while `camex --status` lists the camera: the camera's web server, or
  the firewall's output chain.
- **The start page**: `curl -s -u camex:<password> -H 'Host: openipc.click'
  http://127.0.0.1/` on the server host (the password is in
  `/etc/camex/nginx-password`); a 401 means a wrong password, a 404 that the
  page file is missing: compare `INDEX=` with the block's `root` and run the
  sync script by hand.
- **Everything at once**: `sudo camex-nginx-sync.sh --check` (see [nginx](#nginx)).
- **Through nginx**: `curl -sI -H 'Host: 001a2b3c4d5e.openipc.click'
  http://127.0.0.1/`. The offline page means the name is not in the map:
  compare with `camex --status --format nginx-map --domain openipc.click`
  and run the sync script by hand.
- **Logs**: the server logs `Registered auto client <ID>`, `Expired client
  <address>`, address offers and moves, and rejected handshakes (a wrong key, a
  reserved address); `journalctl -u camex-nginx-sync` shows when the map was
  updated and nginx reloaded; nginx's `error.log` shows connection failures to a
  camera.
