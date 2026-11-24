<!-- Copyright (c) OpenIPC  https://openipc.org  MIT License -->

# camex plugin for the OpenIPC web UI

- `S98camex` → `/etc/init.d/S98camex`: starts the client from the U-Boot environment.
- `plugin-camex.cgi` → `/var/www/cgi-bin/plugin-camex.cgi`: the settings page (Services, Plugins).
- The client itself → `/usr/sbin/camex`.

Variables: `camex_server`, `camex_port` (5800), `camex_pubkey`, `camex_psk`, `camex_name` (the MAC),
`camex_transport` (`udp`), `camex_network`. Those equal to their defaults are not written. The client
starts with `camex_server` and either `camex_network` or `camex_psk` with `camex_pubkey`.

The client for a Cortex-A7 camera (SSC325):

```sh
make -C src TARGET=camex CROSS_COMPILE=arm-linux- LDLIBS=-static PROFILE=client \
    EXTRA_CFLAGS='-mcpu=cortex-a7 -mfpu=neon-vfpv4' all
```
