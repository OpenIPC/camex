<!-- Copyright (c) OpenIPC  https://openipc.org  MIT License -->

# camex documentation

| Document                                 | Contents                                                                                        |
|------------------------------------------|-------------------------------------------------------------------------------------------------|
| [install.md](install.md)                 | `install.sh` on Debian and Ubuntu, installing by hand, the files on the server, clean reinstall |
| [usage.md](usage.md)                     | the command line: keys, modes, client and server, every option, TUN backends, notes             |
| [networks.md](networks.md)               | named networks: joining with the server's address and a name, stable addresses, the server's key on first contact, `--top` |
| [server.md](server.md)                   | `camex.conf`, reloading, `camex --status`, the auto pool and the number of clients              |
| [nginx.md](nginx.md)                     | many cameras behind one server, each web UI by name through nginx; the start page; sync script  |
| [security.md](security.md)               | the protocol, what it does not protect, denial of service limits, recommended deployment        |
| [android.md](android.md)                 | the Android app                                                                                 |
| [android-library.md](android-library.md) | the Android library for apps that build camex in                                                |
| [kernel-module.md](kernel-module.md)     | `camex.ko`: design, building (OpenWrt too), API, limitations                                    |
| [build.md](build.md)                     | requirements, building, cross-compiling, binary sizes                                           |
| [releases.md](releases.md)               | versions, the `latest` release, CI                                                              |

The change history is in [CHANGELOG.md](../CHANGELOG.md).
