<!-- Copyright (c) OpenIPC  https://openipc.org  MIT License -->

# camex examples

The files [install.sh](../install.sh) installs on a server, and where:

| File                       | Installed as                                   | Docs                                                  |
|----------------------------|------------------------------------------------|-------------------------------------------------------|
| `camex.conf`               | `/etc/camex/camex.conf`                        | [server.md](../docs/server.md#the-configuration-file) |
| `camex.service`            | `/etc/systemd/system/camex.service`            | [install.md](../docs/install.md#by-hand)              |
| `camex-nginx-sync.sh`      | `/usr/local/bin/camex-nginx-sync`              | [nginx.md](../docs/nginx.md#sync-script)              |
| `camex-nginx-sync.service` | `/etc/systemd/system/camex-nginx-sync.service` | [nginx.md](../docs/nginx.md#sync-script)              |
| `camex-nginx-sync.timer`   | `/etc/systemd/system/camex-nginx-sync.timer`   | [nginx.md](../docs/nginx.md#sync-script)              |
| `nginx-camex.conf`         | `/etc/nginx/conf.d/camex-nginx.conf`           | [nginx.md](../docs/nginx.md#nginx)                    |

Installing them by hand: [docs/install.md](../docs/install.md#by-hand).
