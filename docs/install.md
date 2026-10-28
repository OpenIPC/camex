<!-- Copyright (c) OpenIPC  https://openipc.org  MIT License -->

# Installing the server

## Debian and Ubuntu: install.sh

[install.sh](../install.sh) installs the camex server in one command:

```sh
curl -fsSL https://raw.githubusercontent.com/OpenIPC/camex/master/install.sh | sudo sh
```

With nginx in front of the clients (see [nginx.md](nginx.md)): each client's
web UI at `http://<client ID>.<domain>/`, the start page of all of them at
`http://<domain>/` behind a password:

```sh
curl -fsSL https://raw.githubusercontent.com/OpenIPC/camex/master/install.sh | sudo sh -s -- --domain openipc.click
```

The steps, printed before it starts and numbered as it goes:

1. Check the host: root, Debian or Ubuntu, systemd, the architecture
   (`amd64`, `arm64`, `armhf`/`armel`, `mipsel`). Other systems are refused
   with a message: install by hand, below.
2. Install the packages it needs with apt-get: `curl`, `ca-certificates`; with
   `--domain` also `nginx` and `openssl`.
3. Download camex of the [`latest`](../README.md#permanent-links) release to
   `/usr/local/bin/camex`.
4. Create the server's key pair, `/etc/camex/server.key` (`camex --genkey`,
   mode 0600).
5. Create the server's settings, `/etc/camex/camex.conf` (mode 0600): the
   port, the auto pool `10.200.0.0/24` with the server at `10.200.0.1`,
   `client_isolation=yes`, `client_timeout=60`, a new random `psk=hex:…` and, with `--domain`,
   `domain=`.
6. Write `/etc/default/camex` (the service's arguments: server mode with that
   config and key), install `camex.service`, enable and start it.
7. With `--domain` (or when nginx was installed before): nginx's server blocks
   `/etc/nginx/conf.d/camex-nginx.conf`, an empty map, the start page's
   password (user `camex`, kept in `/etc/camex/nginx-password`, for nginx in
   `/etc/nginx/camex.htpasswd`), the sync script with its service;
   then `nginx -t`, a reload, one sync and `camex-nginx-sync --check`.
8. Print what the clients need: the server's public key, the psk, a client
   command line and a link for the Android app
   ([android.md](android.md#a-link-with-the-settings)); with `--domain` the
   addresses and the start page's password.

Options:

| Option                      | Meaning                                                                                                                                                                                                              |
|-----------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `--domain <domain>`         | also install nginx in front of the clients for `<domain>`; one in another script (`мойдомен.рф`) is taken in its IDNA form (`xn--d1acklchcc.xn--p1ai`, made by `idn2`), the form of its DNS records and of `domain=` |
| `--port <port>`             | the server's port (5800)                                                                                                                                                                                             |
| `--transport <udp or tcp>`  | the server's transport (`udp`); TCP for networks that block UDP                                                                                                                                                      |
| `--psk <key>`               | the clients' pre-shared key: `hex:<64 hex digits>`, `base64:<44 characters>` or a passphrase (a random `hex:` key)                                                                                                   |
| `--pool <network>/<prefix>` | the clients' pool, `/8` to `/30`; the server gets its first address (`10.200.0.0/24`, the server `10.200.0.1`)                                                                                                       |
| `--client-timeout <s>`      | `client_timeout=` in a new `camex.conf`: seconds of silence before a client is dropped and its lamp turns red, 60 to 120 (60)                                                                                        |
| `--public-host <host>`      | the host name or IPv4 address the clients reach this server at, for the printed command and the Android link (the domain of `camex.conf`, else this host's public address); only printed, nothing kept               |
| `--password <password>`     | with `--domain`: the start page's password, user `camex` (a random one)                                                                                                                                              |
| `--http-port <port>`        | with `--domain`: the port nginx listens on for the cameras and the start page (80); the start page's links are then `http://<ID>.<domain>:<port>/`                                                                   |
| `--upstream-port <port>`    | with `--domain`: the cameras' web port in their tunnel (80)                                                                                                                                                          |
| `--uninstall`               | list every file of camex and the commands to remove them; removes nothing                                                                                                                                            |
| `-h`, `--help`              | the options                                                                                                                                                                                                          |

```sh
curl -fsSL https://raw.githubusercontent.com/OpenIPC/camex/master/install.sh | sudo sh -s -- \
    --domain cams.example.org --transport tcp --port 5900 --pool 10.60.0.0/22 \
    --http-port 85 --upstream-port 8080
```

The options are settings made once, as the files they go to: on a rerun a
different value is not applied; the installer says which line to edit where.
On a server set up by hand before (camex 3.9 and earlier), `--port` or `--psk`
in `CAMEX_ARGS` of `/etc/default/camex` win over `camex.conf`: then those are
compared, and the installer says to edit them there.
A new start page password: remove `/etc/camex/nginx-password` and run it again
with `--password` (or without, for a random one).

`CAMEX_SRC` and `CAMEX_RELEASE` (environment) change where the files of
`examples/` and the binary come from, for tests and mirrors.

### Running it again

A rerun is how camex is updated. It always replaces the binary, the sync
script and the systemd units (a changed file is copied to `/var/backups/camex/`
first) and restarts camex when its binary or unit changed. The settings made
once are kept as they are, edits included: `camex.conf`, `server.key`,
`/etc/default/camex`, nginx's file and the password. Two exceptions:

- nginx added later (`--domain` on a host installed without it): `domain=` is
  added to `[server]` of `camex.conf`.
- The domain is never changed: another `--domain` is refused. Change it by
  hand in `camex.conf` and nginx's file.

A new password for the start page: remove `/etc/camex/nginx-password` and run
`install.sh` again; it replaces the `camex` line of `/etc/nginx/camex.htpasswd`,
other users there stay.

A setup made by `camex-nginx-setup.sh` (camex 3.9 and before) moves to the new
names on the first run: `/etc/nginx/conf.d/camex.conf` becomes
`camex-nginx.conf` with its edits, `/usr/local/bin/camex-nginx-sync.sh` becomes
`camex-nginx-sync`, and the arguments of a `camex.service` that had them in
its `Environment=` line move to `/etc/default/camex`.

## By hand

On other systems, or to do the same steps yourself, with the files of
[examples/](../examples):

```sh
# The binary for this host: camex-x86_64, camex-aarch64, camex-arm or camex-mipsel
ARCH=x86_64
sudo curl -fL -o /usr/local/bin/camex \
    https://github.com/OpenIPC/camex/releases/latest/download/camex-$ARCH
sudo chmod 755 /usr/local/bin/camex

# The key pair (give public_key= to the clients) and the settings
sudo mkdir -p -m 700 /etc/camex
sudo sh -c 'umask 077; camex --genkey > /etc/camex/server.key'
sudo install -m 600 examples/camex.conf /etc/camex/camex.conf
# Edit /etc/camex/camex.conf: psk=, the pool, the clients (see server.md)

# The service
echo 'CAMEX_ARGS="--mode server --config /etc/camex/camex.conf --server-key-file /etc/camex/server.key"' |
    sudo install -m 600 /dev/stdin /etc/default/camex
sudo cp examples/camex.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now camex
```

`camex.service` takes its arguments from `/etc/default/camex`
(`EnvironmentFile=`), so a new version of the unit never loses them;
`systemctl restart camex` after a change. A client under systemd (a PC, a
router):

```sh
CAMEX_ARGS="--mode client --auto --name 0203A104B5AE --server-host vpn.example.org --port 5800 --server-pubkey <server public key> --psk secret"
```

`systemctl reload camex` makes the server read `camex.conf` again
(`SIGHUP`). The unit runs camex with only the capabilities it needs and a
read-only system (`ProtectSystem=strict`); `/run/camex`, the control socket's
directory, is its `RuntimeDirectory=`.

nginx by hand: [nginx.md](nginx.md#nginx).

## Files on the server

Where everything lands on a Linux server. `install.sh --uninstall` prints the
same list, marks the files that are there and the commands to remove them; it
removes nothing itself.

### Installed by install.sh

```
/usr/local/bin/camex                          the binary of the latest release
/etc/camex/camex.conf                         the server's settings, mode 0600
/etc/camex/server.key                         camex --genkey, mode 0600
/etc/default/camex                            CAMEX_ARGS of the service, mode 0600
/etc/systemd/system/camex.service             from examples/camex.service

# With --domain: nginx in front of the clients
/usr/local/bin/camex-nginx-sync               from examples/camex-nginx-sync.sh
/etc/systemd/system/camex-nginx-sync.service  from examples/camex-nginx-sync.service
/etc/nginx/conf.d/camex-nginx.conf            from examples/nginx-camex.conf
/etc/nginx/camex-map.conf                     the map, empty at first
/etc/camex/nginx-password                     the start page's password, mode 0600
/etc/nginx/camex.htpasswd                     the same password for nginx
```

The packages it installs with apt-get: `curl`, `ca-certificates`; with
`--domain` also `nginx` and `openssl`.

### Created at run time

```
/etc/nginx/camex-map.conf.applied             the sync script: the map nginx took
/etc/nginx/camex-map.conf.lock                the sync script: one run at a time
/etc/nginx/camex-map.conf.warnings            the sync script: warnings logged last
/etc/nginx/camex-map.conf.warnings-index
/var/lib/camex-nginx/index.html               the start page, by the sync script
/run/camex/camex-<transport>-<port>.sock      the control socket (gone when camex stops)
/var/backups/camex/                           copies of files install.sh replaced
```

### Left by camex 3.10 and before

`camex-nginx-setup.sh` (3.9 and before) used other names, and camex 3.10 ran
the sync from a timer; `install.sh` moves them to the new ones on its first
run:

```
/usr/local/bin/camex-nginx-sync.sh            now /usr/local/bin/camex-nginx-sync
/etc/nginx/conf.d/camex.conf                  now /etc/nginx/conf.d/camex-nginx.conf
/etc/camex/camex.conf.bak                     a copy of camex.conf before domain= was set
/etc/systemd/system/camex-nginx-sync.timer    stopped and removed (3.10)
```

It could also put nginx's file and the start page elsewhere (`--nginx-conf`,
`--index`): `grep -rl camex /etc/nginx/ /etc/systemd/system/` finds them.

### Not installed by install.sh

```
/lib/modules/$(uname -r)/.../camex.ko         the kernel module: make kmod-install
/etc/modules-load.d/camex.conf                "camex": load it at boot
```

### Clean reinstall

Everything above but the kernel module and the packages, then `install.sh`
again:

```sh
# One unit at a time: systemctl stops at one that is not there
for u in camex-nginx-sync.timer camex-nginx-sync.service camex.service; do
    sudo systemctl disable --now "$u" 2>/dev/null
done
sudo rm -f /usr/local/bin/camex /usr/local/bin/camex-nginx-sync /usr/local/bin/camex-nginx-sync.sh \
    /etc/systemd/system/camex.service /etc/systemd/system/camex-nginx-sync.service \
    /etc/systemd/system/camex-nginx-sync.timer /etc/default/camex \
    /etc/nginx/conf.d/camex-nginx.conf /etc/nginx/conf.d/camex.conf \
    /etc/nginx/camex.htpasswd /etc/nginx/camex-map.conf*
sudo rm -rf /etc/camex /var/lib/camex-nginx /var/backups/camex
sudo systemctl daemon-reload
sudo nginx -t && sudo systemctl reload nginx
```

- `/etc/camex/server.key` and `psk=` in `camex.conf` go too: the new install
  makes new ones, and every client then needs the new public key and psk. To
  keep them, save both files first and put them back in `/etc/camex/` before
  running `install.sh`: it keeps files that are there.
- The packages stay; `sudo apt-get purge nginx nginx-common` if nothing else
  uses nginx.

The scripts in `docker/`, `tests/` and `ci/` are for building and testing only.