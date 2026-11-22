<!-- Copyright (c) OpenIPC  https://openipc.org  MIT License -->

# camex examples

The files [install.sh](../install.sh) installs on a server, and where:

| File               | Installed as                         | Docs                                                  |
|--------------------|--------------------------------------|-------------------------------------------------------|
| `camex.conf`       | `/etc/camex/camex.conf`              | [server.md](../docs/server.md#the-configuration-file) |
| `camex.service`    | `/etc/systemd/system/camex.service`  | [install.md](../docs/install.md#by-hand)              |
| `nginx-camex.conf` | `/etc/nginx/conf.d/camex-nginx.conf` | [nginx.md](../docs/nginx.md#nginx)                    |

Installing them by hand: [docs/install.md](../docs/install.md#by-hand).

Deprecated, for servers set up by camex 5.1.0 and before (nginx with a map
file); `install.sh` keeps them where they are, camex 6.0.0 removes them
([nginx.md](../docs/nginx.md#moving-to-web_listen)):

| File                       | Installed as                                   | Docs                                                |
|----------------------------|------------------------------------------------|-----------------------------------------------------|
| `camex-nginx-sync.sh`      | `/usr/local/bin/camex-nginx-sync`              | [nginx.md](../docs/nginx.md#sync-script-deprecated) |
| `camex-nginx-sync.service` | `/etc/systemd/system/camex-nginx-sync.service` | [nginx.md](../docs/nginx.md#sync-script-deprecated) |
| `nginx-camex-map.conf`     | `/etc/nginx/conf.d/camex-nginx.conf`           | [nginx.md](../docs/nginx.md#sync-script-deprecated) |
