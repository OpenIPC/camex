<!-- Copyright (c) OpenIPC  https://openipc.org  MIT License -->

![OpenIPC logo][logo]

# OpenIPC Camex

[English](README.md) | **Русский** | [简体中文](README.zh-CN.md)

[![camex CI](https://github.com/OpenIPC/camex/actions/workflows/build.yml/badge.svg)](https://github.com/OpenIPC/camex/actions/workflows/build.yml)
[![Release](https://img.shields.io/badge/download-latest-blue)](https://github.com/OpenIPC/camex/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Code size](https://img.shields.io/github/languages/code-size/OpenIPC/camex)](https://github.com/OpenIPC/camex)

Минималистичный UDP/TCP-туннель без внешних зависимостей для встраиваемых Linux-систем.

Поставляется в виде демона пользовательского пространства и опционального автономного модуля ядра.

Камеры и другие устройства за NAT сами подключаются к одному серверу camex;
сервер выдаёт каждому туннельный адрес, а с nginx перед ними открывает
веб-интерфейс каждого устройства по имени, со стартовой страницей всех устройств.

---

## Возможности

- **Архитектура клиент/сервер** поверх UDP или TCP
- **Режим автоконфигурации** — клиент получает параметры туннеля от сервера
- **Протокол v2** — рукопожатие Noise (`NKpsk0`, X25519, ChaCha20-Poly1305,
  BLAKE2b), новые ключи на каждую сессию и направление, шифрование всегда
- **Без внешних зависимостей** — нужна только libc
- **Кроссплатформенная сборка** — x86_64, ARM, AArch64, MIPS (через CI)
- **Собственный модуль ядра** — `camex.ko` заменяет `tun.ko` и не имеет внешних зависимостей
- **Компактный код** — всего ~3 500 строк, легко провести аудит

## Установка

### Сервер на Debian или Ubuntu

Одна команда ставит сервер: camex из релиза `latest`, его ключевую пару,
`/etc/camex/camex.conf` с новым предварительным ключом и службу systemd.
В конце она выводит то, что нужно клиентам:

```sh
curl -fsSL https://raw.githubusercontent.com/OpenIPC/camex/master/install.sh | sudo sh
```

С `--domain` ставится ещё и nginx перед клиентами: веб-интерфейс каждого
клиента на `http://<ID клиента>.<домен>/`, стартовая страница на
`http://<домен>/`:

```sh
curl -fsSL https://raw.githubusercontent.com/OpenIPC/camex/master/install.sh | sudo sh -s -- --domain openipc.click
```

Повторный запуск обновляет camex; `--uninstall` выводит все файлы camex и как
их удалить. Шаги, файлы на сервере, ручная установка на других системах:
[docs/install.md](docs/install.md) (на английском).

### Клиент

Камера (или любое Linux-устройство) с `camex-<arch>` из
[постоянных ссылок](#постоянные-ссылки), с публичным ключом и psk, которые
вывел установщик:

```sh
camex --mode client --auto --name 0203A104B5AE \
  --server-host vpn.example.org --port 5800 \
  --server-pubkey <public_key> --psk <psk>
```

Все опции: [docs/usage.md](docs/usage.md).

## Скриншоты

### Android

Приложение подключается в автоматическом режиме: адрес, маршруты и MTU присылает
сервер ([docs/android.md](docs/android.md)).

| Главный экран                                  | Настройки                                      |
|------------------------------------------------|------------------------------------------------|
| ![Главный экран](docs/images/android-main.png) | ![Настройки](docs/images/android-settings.png) |

### Веб

Стартовая страница устройств на `http://<домен>/`; каждое имя — ссылка на
веб-интерфейс устройства `http://<ID клиента>.<домен>/`
([docs/nginx.md](docs/nginx.md)).

| Стартовая страница                                      | На телефоне                                                               |
|---------------------------------------------------------|---------------------------------------------------------------------------|
| ![Стартовая страница](docs/images/nginx-start-page.png) | ![Стартовая страница на телефоне](docs/images/nginx-start-page-phone.png) |

## Постоянные ссылки

Бинарные файлы публикуются одним релизом, [`latest`](https://github.com/OpenIPC/camex/releases/latest),
который CI пересобирает при каждом push в `master`. В его заголовке — версия camex
(`Camex X.Y.Z`); в именах файлов версии нет, поэтому ссылки не меняются:

- страница релиза: `https://github.com/OpenIPC/camex/releases/latest`
- файл: `https://github.com/OpenIPC/camex/releases/latest/download/<файл>`

| Файл                       | Платформа                                                                        |
|----------------------------|----------------------------------------------------------------------------------|
| `camex-aarch64`            | Linux AArch64, статически (musl)                                                 |
| `camex-arm`                | Linux 32-bit ARM, статически (musl): камеры OpenIPC                              |
| `camex-mipsel`             | Linux MIPS little-endian, статически (musl, soft-float): камеры Ingenic, роутеры |
| `camex-x86_64`             | Linux x86_64, статически (musl)                                                  |
| `camex-android.apk`        | Android 8.0 и новее                                                              |
| `camex-macos.zip`          | macOS (utun): `camex`, LICENSE, README                                           |
| `camex-windows-x86_64.zip` | Windows 64-bit: `camex.exe` и `wintun.dll`                                       |
| `camex-windows-i686.zip`   | Windows 32-bit: `camex.exe` и `wintun.dll`                                       |

```sh
URL=https://github.com/OpenIPC/camex/releases/latest/download
curl -fL -O $URL/camex-arm
```

История изменений — в [CHANGELOG.md](CHANGELOG.md).

## Документация

Документация в `docs/` — на английском.

| Документ                                | Содержание                                                                       |
|-----------------------------------------|----------------------------------------------------------------------------------|
| [Установка](docs/install.md)            | `install.sh`, ручная установка, файлы на сервере, очистка для чистой установки   |
| [Использование](docs/usage.md)          | командная строка: ключи, режимы, все опции, драйверы TUN                         |
| [Конфигурация сервера](docs/server.md)  | `camex.conf`, `camex --status`, автоматический пул и число клиентов              |
| [nginx перед клиентами](docs/nginx.md)  | веб-интерфейс каждого устройства по имени, стартовая страница, файрвол, DNS, TLS |
| [Модель безопасности](docs/security.md) | протокол, его границы, рекомендуемое развёртывание                               |
| [Приложение Android](docs/android.md)   | настройки, VPN и маршруты                                                        |
| [Модуль ядра](docs/kernel-module.md)    | `camex.ko`: устройство, сборка (и для OpenWrt), API, ограничения                 |
| [Сборка](docs/build.md)                 | требования, сборка, кросс-компиляция, размер бинарных файлов                     |
| [Версии и релизы](docs/releases.md)     | версии, релиз `latest`, CI                                                       |

## Лицензия

[MIT](LICENSE). Полный текст условий — в файле [LICENSE](LICENSE).

## Поддержка

- Вопросы и сообщения об ошибках: [GitHub Issues](https://github.com/OpenIPC/camex/issues)
- Документация: [openipc.org](https://openipc.org)

[![Telegram](https://openipc.org/images/telegram_button.svg)](https://openipc.org/our-channels)

[logo]: https://raw.githubusercontent.com/OpenIPC/.github/refs/heads/main/assets/openipc-logo-black.svg
