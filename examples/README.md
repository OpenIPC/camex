<!-- Copyright (c) OpenIPC  https://openipc.org  MIT License -->

# camex examples

The files [install.sh](../install.sh) installs on a server, and where:

| File               | Installed as                         | Docs                                                  |
|--------------------|--------------------------------------|-------------------------------------------------------|
| `camex.conf`       | `/etc/camex/camex.conf`              | [server.md](../docs/server.md#the-configuration-file) |
| `camex.service`    | `/etc/systemd/system/camex.service`  | [install.md](../docs/install.md#by-hand)              |
| `nginx-camex.conf` | `/etc/nginx/conf.d/camex-nginx.conf` | [nginx.md](../docs/nginx.md#nginx)                    |

[openipc-webui/](openipc-webui/) is for a camera, not a server: the client's page in the OpenIPC web UI
and its init script.

Installing them by hand: [docs/install.md](../docs/install.md#by-hand).

camex 6.0.0 removed the sync script, its service and nginx's file of the map
(servers set up by camex 5.1.0 and before):
[nginx.md](../docs/nginx.md#moving-from-the-map) says how such a server moves
over.
