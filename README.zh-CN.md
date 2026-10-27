<!-- Copyright (c) OpenIPC  https://openipc.org  MIT License -->

![OpenIPC logo][logo]

# OpenIPC Camex

[English](README.md) | [Русский](README.ru.md) | **简体中文**

[![camex CI](https://github.com/OpenIPC/camex/actions/workflows/build.yml/badge.svg)](https://github.com/OpenIPC/camex/actions/workflows/build.yml)
[![Release](https://img.shields.io/badge/download-latest-blue)](https://github.com/OpenIPC/camex/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Code size](https://img.shields.io/github/languages/code-size/OpenIPC/camex)](https://github.com/OpenIPC/camex)

面向嵌入式 Linux 目标设备的极简、无外部依赖的 UDP/TCP 隧道。

以用户态守护进程的形式提供，并附带一个可选的独立内核模块。

位于 NAT 之后的摄像头及其他设备主动连接到同一台 camex 服务器；服务器为每台设备
分配隧道地址，并在前面配合 nginx 按名称打开每台设备的 Web 界面，还提供列出所有
设备的起始页。

---

## 功能特性

- **客户端/服务器架构**，基于 UDP 或 TCP
- **自动配置模式** —— 客户端从服务器获取隧道参数
- **协议 v2** —— Noise 握手（`NKpsk0`、X25519、ChaCha20-Poly1305、BLAKE2b），
  每个会话、每个方向使用新的密钥，始终加密
- **无外部依赖** —— 仅需 libc
- **跨平台构建** —— x86_64、ARM、AArch64、MIPS（通过 CI）
- **自研内核模块** —— `camex.ko` 可替代 `tun.ko`，无外部依赖
- **代码精简** —— 总计约 3,500 行，便于审计

## 安装

### Debian 或 Ubuntu 上的服务器

一条命令即可安装服务器：`latest` 发布版的 camex、它的密钥对、带有新预共享密钥的
`/etc/camex/camex.conf` 以及 systemd 服务。最后它会输出客户端所需的信息：

```sh
curl -fsSL https://raw.githubusercontent.com/OpenIPC/camex/master/install.sh | sudo sh
```

加上 `--domain` 时还会在客户端前安装 nginx：每个客户端的 Web 界面位于
`http://<客户端 ID>.<域名>/`，起始页位于 `http://<域名>/`：

```sh
curl -fsSL https://raw.githubusercontent.com/OpenIPC/camex/master/install.sh | sudo sh -s -- --domain openipc.click
```

再次运行即可更新 camex；`--uninstall` 列出 camex 的所有文件及删除方法。安装步骤、
服务器上的文件以及在其他系统上手动安装：[docs/install.md](docs/install.md)（英文）。

### 客户端

摄像头（或任何 Linux 设备）使用[永久链接](#永久链接)中的 `camex-<arch>`，以及安装
程序输出的公钥和 psk：

```sh
camex --mode client --auto --name 0203A104B5AE \
  --server-host vpn.example.org --port 5800 \
  --server-pubkey <public_key> --psk <psk>
```

全部选项：[docs/usage.md](docs/usage.md)。

## 截图

### Android

应用以自动模式连接：地址、路由和 MTU 由服务器下发（[docs/android.md](docs/android.md)）。

| 主界面                                  | 设置                                      |
|-----------------------------------------|-------------------------------------------|
| ![主界面](docs/images/android-main.png) | ![设置](docs/images/android-settings.png) |

### Web

位于 `http://<域名>/` 的设备起始页，每个名称都链接到该设备的 Web 界面
`http://<客户端 ID>.<域名>/`（[docs/nginx.md](docs/nginx.md)）。

| 起始页                                      | 手机上                                                    |
|---------------------------------------------|-----------------------------------------------------------|
| ![起始页](docs/images/nginx-start-page.png) | ![手机上的起始页](docs/images/nginx-start-page-phone.png) |

## 永久链接

二进制文件只发布在一个版本中：[`latest`](https://github.com/OpenIPC/camex/releases/latest)，
CI 会在每次推送到 `master` 时重新构建。其标题显示 camex 版本（`Camex X.Y.Z`）；
文件名不含版本号，因此以下链接始终不变：

- 发布页面：`https://github.com/OpenIPC/camex/releases/latest`
- 单个文件：`https://github.com/OpenIPC/camex/releases/latest/download/<文件>`

| 文件                       | 平台                                                          |
|----------------------------|---------------------------------------------------------------|
| `camex-aarch64`            | Linux AArch64，静态链接（musl）                               |
| `camex-arm`                | Linux 32 位 ARM，静态链接（musl）：OpenIPC 摄像头             |
| `camex-mipsel`             | Linux MIPS 小端，静态链接（musl，软浮点）：君正摄像头、路由器 |
| `camex-x86_64`             | Linux x86_64，静态链接（musl）                                |
| `camex-android.apk`        | Android 8.0 及更高版本                                        |
| `camex-macos.zip`          | macOS（utun）：`camex`、LICENSE、README                       |
| `camex-windows-x86_64.zip` | Windows 64 位：`camex.exe` 和 `wintun.dll`                    |
| `camex-windows-i686.zip`   | Windows 32 位：`camex.exe` 和 `wintun.dll`                    |

```sh
URL=https://github.com/OpenIPC/camex/releases/latest/download
curl -fL -O $URL/camex-arm
```

变更历史见 [CHANGELOG.md](CHANGELOG.md)。

## 文档

`docs/` 中的文档为英文。

| 文档                                | 内容                                                     |
|-------------------------------------|----------------------------------------------------------|
| [安装](docs/install.md)             | `install.sh`、手动安装、服务器上的文件、全新重装前的清理 |
| [使用](docs/usage.md)               | 命令行：密钥、模式、全部选项、TUN 驱动                   |
| [服务器配置](docs/server.md)        | `camex.conf`、`camex --status`、自动地址池与客户端数量   |
| [客户端前面的 nginx](docs/nginx.md) | 按名称访问每台设备的 Web 界面、起始页、防火墙、DNS、TLS  |
| [安全模型](docs/security.md)        | 协议、其局限、推荐部署                                   |
| [Android 应用](docs/android.md)     | 设置、VPN 与路由                                         |
| [内核模块](docs/kernel-module.md)   | `camex.ko`：设计、构建（含 OpenWrt）、API、限制          |
| [构建](docs/build.md)               | 系统要求、构建、交叉编译、二进制大小                     |
| [版本与发布](docs/releases.md)      | 版本、`latest` 发布、CI                                  |

## 许可证

[MIT](LICENSE)。完整条款请参见 [LICENSE](LICENSE)。

## 支持

- 问题咨询与缺陷报告：[GitHub Issues](https://github.com/OpenIPC/camex/issues)
- 文档：[openipc.org](https://openipc.org)

[![Telegram](https://openipc.org/images/telegram_button.svg)](https://openipc.org/our-channels)

[logo]: https://raw.githubusercontent.com/OpenIPC/.github/refs/heads/main/assets/openipc-logo-black.svg
