# Camex

This directory contains example files for deploying and configuring
camex in various environments.

## Contents

| File                       | Description                                                                                                                                                                     |
|----------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `camex.conf`               | Server-side configuration file format reference                                                                                                                                 |
| `camex.service`            | systemd service unit for running camex as a daemon                                                                                                                              |
| `camex-nginx-sync.sh`      | Writes the nginx map of the live clients (`camex --status --format nginx-map`) and reloads nginx when it changed; with `INDEX=`, also their start page (`--format nginx-index`) |
| `camex-nginx-setup.sh`     | Sets up nginx in front of the clients with one domain, given once: server blocks, sync script, service, timer, `domain=` in `camex.conf`; then checks it                        |
| `camex-nginx-sync.service` | systemd unit running the sync script once                                                                                                                                       |
| `camex-nginx-sync.timer`   | systemd timer starting the sync every 10 seconds                                                                                                                                |
| `nginx-camex.conf`         | nginx server blocks: each client's web UI by name, an offline page, the start page on the bare domain                                                                           |

## Usage

### Server configuration

```sh
sudo mkdir -p /etc/camex
sudo cp examples/camex.conf /etc/camex/camex.conf
# Edit /etc/camex/camex.conf with your client profiles
# The server key pair (once); give public_key= to the clients:
sudo sh -c 'umask 077; camex --genkey > /etc/camex/server.key'
sudo camex --mode server --port 5800 --config /etc/camex/camex.conf \
    --server-key-file /etc/camex/server.key --psk secret
```

### systemd service

```sh
sudo cp examples/camex.service /etc/systemd/system/
sudo systemctl daemon-reload
# Edit the Environment line in the service file for your mode
sudo systemctl enable camex
sudo systemctl start camex
```

### nginx in front of the clients

Each client's web UI at `http://<client ID>.<domain>/` and a start page of
all devices at `http://<domain>/`, with the domain given once:

```sh
# Installs the nginx server blocks, the sync script, its service and timer;
# sets domain= in /etc/camex/camex.conf and reloads the camex servers; the
# start page's password (user camex) is printed and kept in
# /etc/camex/nginx-password (--new-password makes a new one)
sudo examples/camex-nginx-setup.sh --domain openipc.click
# Checks the whole setup at any time: servers, domain, nginx, start page, DNS
sudo camex-nginx-sync.sh --check
```

DNS records for `*.<domain>` and `<domain>`, TLS, the firewall and the steps
by hand are in [docs/nginx.md](../docs/nginx.md).
