<!-- Copyright (c) OpenIPC  https://openipc.org  MIT License -->

# Android app

`camex-android.apk` of the [`latest`](../README.md#permanent-links) release,
Android 8.0 and later. The app connects in auto mode: the server sends the
address, routes and MTU.

| Main screen                             | Settings                                 |
|-----------------------------------------|------------------------------------------|
| ![Main screen](images/android-main.png) | ![Settings](images/android-settings.png) |

## Settings

- **Server host / port** — the camex server.
- **Client name** — the client ID, i.e. the `[client ID]` block on the server.
- **Transport** — UDP or TCP, as on the server.
- **Server public key** and **Pre-shared key** — `--server-pubkey --psk`; the
  key must match the client's `psk=` (or the global key).
- **Route all traffic through the tunnel** — off: only the tunnel network and
  the server's routes go through the tunnel.
- **Advanced** — MTU, keepalive and server timeout.

![Advanced settings](images/android-settings-advanced.png)

## VPN and routes

The app uses `VpnService`, so Android shows the VPN key icon while the tunnel
is up. What goes through the tunnel and what does not:
[security.md](security.md#android-app).
