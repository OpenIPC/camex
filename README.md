<!-- Copyright (c) OpenIPC  https://openipc.org  MIT License -->

![OpenIPC logo][logo]

# OpenIPC Camex

**English** | [Русский](README.ru.md) | [简体中文](README.zh-CN.md)

[![camex CI](https://github.com/OpenIPC/camex/actions/workflows/build.yml/badge.svg)](https://github.com/OpenIPC/camex/actions/workflows/build.yml)
[![Release](https://img.shields.io/badge/download-latest-blue)](https://github.com/OpenIPC/camex/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Code size](https://img.shields.io/github/languages/code-size/OpenIPC/camex)](https://github.com/OpenIPC/camex)

Minimal dependency-free UDP/TCP tunnel for embedded Linux targets.

Ships as a userspace daemon and an optional standalone kernel module.

Cameras and other devices behind NAT connect out to one camex server; the
server gives each a tunnel address and, with nginx in front of them, opens each
device's web UI by name, with a start page of all of them.

---

## Features

- **Client/server architecture** over UDP or TCP
- **Auto-config mode** — client fetches tunnel parameters from the server
- **Protocol v2** — Noise handshake (`NKpsk0`, X25519, ChaCha20-Poly1305,
  BLAKE2b), fresh keys per session and direction, always encrypted
- **No external dependencies** — only libc required
- **Cross-platform builds** — x86_64, ARM, AArch64, MIPS (via CI)
- **Custom kernel module** — `camex.ko` replaces `tun.ko` with no external deps
- **Compact codebase** — ~3,500 lines total, easy to audit

## Installation

### Server on Debian or Ubuntu

One command installs the server: camex of the `latest` release, its key pair,
`/etc/camex/camex.conf` with a new pre-shared key, and the systemd service.
At the end it prints what the clients need:

```sh
curl -fsSL https://raw.githubusercontent.com/OpenIPC/camex/master/install.sh | sudo sh
```

With `--domain`, nginx in front of the clients too: each client's web UI at
`http://<client ID>.<domain>/`, the start page at `http://<domain>/`:

```sh
curl -fsSL https://raw.githubusercontent.com/OpenIPC/camex/master/install.sh | sudo sh -s -- --domain openipc.click
```

Run it again to update camex; `--uninstall` lists every file of camex and how
to remove them. The steps, the files on the server, installing by hand on other
systems: [docs/install.md](docs/install.md).

### Client

A camera (or any Linux device) with `camex-<arch>` from
[Permanent links](#permanent-links), with the public key and psk the installer
printed:

```sh
camex --mode client --auto --name 0203A104B5AE \
  --server-host vpn.example.org --port 5800 \
  --server-pubkey <public_key> --psk <psk>
```

Every option: [docs/usage.md](docs/usage.md).

## Screenshots

### Android

The app connects in auto mode: the server sends the address, routes and MTU
([docs/android.md](docs/android.md)). `install.sh` prints a link with the
server's settings: tapped on the phone, it fills them in on the app's settings
screen, and they are used once you tap Save
([docs/android.md](docs/android.md#a-link-with-the-settings)).

| Main screen                                  | Settings                                      |
|----------------------------------------------|-----------------------------------------------|
| ![Main screen](docs/images/android-main.png) | ![Settings](docs/images/android-settings.png) |

### Web

The start page of the devices at `http://<domain>/`, each name a link to the
device's web UI at `http://<client ID>.<domain>/`
([docs/nginx.md](docs/nginx.md)).

| Start page                                      | On a phone                                                       |
|-------------------------------------------------|------------------------------------------------------------------|
| ![Start page](docs/images/nginx-start-page.png) | ![Start page on a phone](docs/images/nginx-start-page-phone.png) |

## Permanent links

The binaries are published as one release, [`latest`](https://github.com/OpenIPC/camex/releases/latest),
rebuilt by CI from every push to `master`. Its title shows the camex version
(`Camex X.Y.Z`); the file names carry no version, so these links never change:

- release page: `https://github.com/OpenIPC/camex/releases/latest`
- a file: `https://github.com/OpenIPC/camex/releases/latest/download/<file>`

| File                       | Platform                                                                      |
|----------------------------|-------------------------------------------------------------------------------|
| `camex-aarch64`            | Linux AArch64, static (musl)                                                  |
| `camex-arm`                | Linux 32-bit ARM, static (musl): OpenIPC cameras                              |
| `camex-mipsel`             | Linux MIPS little-endian, static (musl, soft-float): Ingenic cameras, routers |
| `camex-x86_64`             | Linux x86_64, static (musl)                                                   |
| `camex-android.apk`        | Android 8.0 and later                                                         |
| `camex-macos.zip`          | macOS (utun): `camex`, LICENSE, README                                        |
| `camex-windows-x86_64.zip` | Windows 64-bit: `camex.exe` and `wintun.dll`                                  |
| `camex-windows-i686.zip`   | Windows 32-bit: `camex.exe` and `wintun.dll`                                  |

```sh
URL=https://github.com/OpenIPC/camex/releases/latest/download
curl -fL -O $URL/camex-arm
```

The change history is in [CHANGELOG.md](CHANGELOG.md).

## Documentation

| Document                                       | Contents                                                                   |
|------------------------------------------------|----------------------------------------------------------------------------|
| [Installation](docs/install.md)                | `install.sh`, installing by hand, the files on the server, clean reinstall |
| [Usage](docs/usage.md)                         | the command line: keys, modes, every option, TUN backends                  |
| [Server configuration](docs/server.md)         | `camex.conf`, `camex --status`, the auto pool and the number of clients    |
| [nginx in front of the clients](docs/nginx.md) | each device's web UI by name, the start page, firewall, DNS, TLS           |
| [Security model](docs/security.md)             | the protocol, its limits, recommended deployment                           |
| [Android app](docs/android.md)                 | settings, VPN and routes                                                   |
| [Kernel module](docs/kernel-module.md)         | `camex.ko`: design, building (OpenWrt too), API, limitations               |
| [Building](docs/build.md)                      | requirements, building, cross-compiling, binary sizes                      |
| [Versions and releases](docs/releases.md)      | versions, the `latest` release, CI                                         |

## License

[MIT](LICENSE). See [LICENSE](LICENSE) for the full terms.

## Support

- Questions and bug reports: [GitHub Issues](https://github.com/OpenIPC/camex/issues)
- Documentation: [openipc.org](https://openipc.org)

[![Telegram](https://openipc.org/images/telegram_button.svg)](https://openipc.org/our-channels)

[logo]: https://raw.githubusercontent.com/OpenIPC/.github/refs/heads/main/assets/openipc-logo-black.svg
