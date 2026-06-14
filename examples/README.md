#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# examples/ — reference configurations, service units, and helpers
#

This directory contains example files for deploying and configuring
camex in various environments.

## Contents

| File | Description |
|------|-------------|
| `camex.conf` | Server-side configuration file format reference |
| `camex.service` | systemd service unit for running camex as a daemon |

## Usage

### Server configuration

```sh
sudo mkdir -p /etc/camex
sudo cp examples/camex.conf /etc/camex/camex.conf
# Edit /etc/camex/camex.conf with your client profiles
sudo camex --mode server --port 5800 --config /etc/camex/camex.conf
```

### systemd service

```sh
sudo cp examples/camex.service /etc/systemd/system/
sudo systemctl daemon-reload
# Edit the Environment line in the service file for your mode
sudo systemctl enable camex
sudo systemctl start camex
```
