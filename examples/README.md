# Camex

This directory contains example files for deploying and configuring
camex in various environments.

## Contents

| File                       | Description                                                                                                                                                                     |
|----------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `camex.conf`               | Server-side configuration file format reference                                                                                                                                 |
| `camex.service`            | systemd service unit for running camex as a daemon                                                                                                                              |
| `camex-nginx-sync.sh`      | Writes the nginx map of the live clients (`camex --status --format nginx-map`) and reloads nginx when it changed; with `INDEX=`, also their start page (`--format nginx-index`) |
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
