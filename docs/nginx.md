# Deployment with nginx: many cameras, one server, each web UI by name

This guide sets up one Linux host that runs the camex server and nginx side by
side, for many cameras (or other devices) behind NAT. Each camera connects with
camex in auto mode, gets a tunnel address from the server's pool, and its web UI
opens through nginx by name:

```
http://<camera ID>.cam.example.lan/   →   http://<its tunnel address>:80/
```

Nobody keeps a list of addresses: nginx follows the server's live state through
`camex --status --format nginx-map` and a small sync script.

## Topology

```
 cameras (behind NAT)                    server host (Linux)
 ┌─────────────────────┐   UDP 5800    ┌─────────────────────────────────────┐
 │ camex --auto        │ ────────────▶ │ camex --mode server                 │
 │   --name <MAC>      │               │   tunnel interface camex/tun0       │
 │ web UI on :80 of    │ ◀── tunnel ── │   pool 10.200.0.0/22                │
 │ its tunnel address  │               │                                     │
 └─────────────────────┘               │ nginx :80/:443 *.cam.example.lan    │
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
  reloads nginx.

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
psk=hex:<this camera's 64 hex digits>
```

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
  (`001a2b3c4d5e.cam.example.lan`). IDs with `:` or `_` are left out of the map
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

1. **DNS**: a wildcard record `*.cam.example.lan` pointing to the server host.
2. **TLS** (optional): a wildcard certificate for `*.cam.example.lan`; uncomment
   the `listen 443 ssl` lines in the example.
3. **Server block**: copy `examples/nginx-camex.conf` to `/etc/nginx/conf.d/`
   and replace `cam.example.lan` with your domain. It includes the map, proxies
   to the camera with WebSocket support and without buffering, gives up on a
   camera after 5 s and shows an "offline" page for names with no live camera.
4. **The map**: create an empty one so that nginx starts, then let the sync
   script keep it current:

   ```sh
   printf 'map $host $camex_upstream {\n}\n' | sudo tee /etc/nginx/camex-map.conf
   sudo cp examples/camex-nginx-sync.sh /usr/local/bin/
   sudo cp examples/camex-nginx-sync.service examples/camex-nginx-sync.timer \
       /etc/systemd/system/
   # Set DOMAIN= in the service, then:
   sudo systemctl daemon-reload
   sudo systemctl enable --now camex-nginx-sync.timer
   ```

   The script writes `camex --status --format nginx-map` to the map only when
   it changed, runs `nginx -t` and reloads nginx, and puts the previous map back
   if nginx rejects the new one; runs never overlap. When a running server does
   not answer it leaves the map as it is (a socket left behind by a server that
   is gone is skipped).

The map looks like this:

```
# camex --status --format nginx-map: generated, do not edit
map $host $camex_upstream {
    "001a2b3c4d5e.cam.example.lan" 10.200.0.2:80;
    "001a2b3c4d5f.cam.example.lan" 10.200.0.3:80;
}
```

## Checks

- **Who is connected**: `sudo camex --status` lists each camera with its ID,
  address, peer, last packet and traffic, and the addresses kept for cameras
  that are gone; `--json` gives the same for scripts and monitoring.
- **A camera directly**: `curl -sI http://10.200.0.2/` on the server host. No
  answer while `camex --status` lists the camera: the camera's web server, or
  the firewall's output chain.
- **Through nginx**: `curl -sI -H 'Host: 001a2b3c4d5e.cam.example.lan'
  http://127.0.0.1/`. The offline page means the name is not in the map:
  compare with `camex --status --format nginx-map --domain cam.example.lan`
  and run the sync script by hand.
- **Logs**: the server logs `Registered auto client <ID>`, `Expired client
  <address>`, address offers and moves, and rejected handshakes (a wrong key, a
  reserved address); `journalctl -u camex-nginx-sync` shows when the map was
  updated and nginx reloaded; nginx's `error.log` shows connection failures to a
  camera.
