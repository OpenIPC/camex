<!-- Copyright (c) OpenIPC  https://openipc.org  MIT License -->

# Android library: camex inside an app

`android/tunnel` is an Android library module (MIT) for apps that build the camex
client in, so they join a camex server and reach its other clients without the
camex app (#170). It runs camex in-process as a VPN client in auto mode, as
the [camex app](android.md) does (the app itself is built on it since 3.19.0,
#172), with these differences:

- **The app stays inside its own VPN.** camex's own sockets (the server
  connection) are protected from the VPN (`VpnService.protect()`); the app's
  sockets go through the tunnel, both ways: the app connects to other clients,
  and other clients connect to the app's listening sockets (`0.0.0.0:8080`).
- **Only the tunnel subnet and the server's routes** go through the VPN; a
  default route (`0.0.0.0/0`) from the server is never taken, nor a route wider
  than a `/8` (#175).
- **No Activity, no stored settings.** The app keeps its settings, shows its
  own screens and passes the values to `CamexTunnel.start()`.
- **It reconnects until stopped.** camex reconnects to its server by itself;
  when camex exits, the library starts it again after 1 s, doubling up to
  60 s, sooner when a network comes up. Only `stop()` ends it.

minSdk 21, no product flavors, Java 8 API, no AndroidX at runtime.

## Adding it to an app

Take camex as a git submodule pinned to a release commit (the library's
version is camex's, `src/version.h`, `CamexTunnel.VERSION`):

```sh
git submodule add https://github.com/OpenIPC/camex bundles/camex
```

`settings.gradle`:

```groovy
include ':camex-tunnel'
project(':camex-tunnel').projectDir = file('../bundles/camex/android/tunnel')
```

The root project puts the Android Gradle plugin on the classpath (an
`com.android.library` plugin declared with `apply false`, or the application
plugin of the same version). The app module:

```groovy
dependencies {
    implementation project(':camex-tunnel')
}
```

**ABIs come from the app.** The library sets no `abiFilters` of its own: the
APK gets the app's ABIs. To build the native code only for those, set
`camexTunnelAbis` in the root project, e.g. from the app's own ABI list:

```groovy
ext.camexTunnelAbis = ['arm64-v8a', 'armeabi-v7a']   // or 'arm64-v8a,armeabi-v7a'; an empty or unknown ABI is a build error
```

Keep `ndkVersion` in the app module, so `libcamex.so` is stripped when the
APK is packed. The library's consumer R8 rules keep the methods the native
code calls.

The library's manifest adds the VPN service and the permissions `INTERNET`,
`ACCESS_NETWORK_STATE`, `FOREGROUND_SERVICE`, `FOREGROUND_SERVICE_SPECIAL_USE`
and `POST_NOTIFICATIONS` (the app asks for the last one at run time on
Android 13 and later).

## Using it

```java
import org.openipc.camex.tunnel.CamexTunnel;

Map<String, String> v = new HashMap<>();
v.put(CamexTunnel.SERVER, "vpn.example.org:5800");
v.put(CamexTunnel.NAME, "box-1");               // the client ID on the server
v.put(CamexTunnel.TRANSPORT, "UDP");            // or "TCP"
v.put(CamexTunnel.PUBKEY, "<server public key>");
v.put(CamexTunnel.PSK, "<psk>");

String[] bad = CamexTunnel.check(v);            // null, or {key, reason}
CamexTunnel.start(context, v);                  // false: see state() and lastError()
...
CamexTunnel.stop();
```

| Call                                  | What it does                                                                                                |
|---------------------------------------|-------------------------------------------------------------------------------------------------------------|
| `fields()`                            | the settings, in the order of a settings screen: key, label, kind, initial value, choices, required         |
| `check(values)`                       | null when the values are fine, otherwise `{key, reason}` of the first wrong field                           |
| `consentIntent(context)`              | the system's VPN consent dialog to show, or null when the app has the consent (`VpnService.prepare`)        |
| `start(context, values)`              | starts the tunnel, or restarts it with new values; never throws for a missing consent or bad values         |
| `stop()`                              | stops the tunnel and its reconnects                                                                         |
| `state()`, `address()`, `lastError()` | the state, the app's address in the tunnel while connected, a short reason of the last failure or reconnect |
| `running()`                           | true while the tunnel connects or is up                                                                     |
| `addListener(l)`, `removeListener(l)` | `onState(state, address, lastError)` on the main thread, on every change and once when added                |
| `setNotificationLook(title, icon)`    | the title and small icon of the library's notification                                                      |
| `setNotification(id, factory)`        | the app's own notification (and id) instead of the library's                                                |
| `ConnectLink.parse(link)`             | a camex link to the values, see below                                                                       |

The values (`fields()`):

| Key              | Kind      | Value                                                                                                                |
|------------------|-----------|----------------------------------------------------------------------------------------------------------------------|
| `server`         | host:port | the camex server: a host name or an IPv4 address and its port                                                        |
| `name`           | text      | the client ID on the server (`[client ID]`), 1 to 31 bytes                                                           |
| `transport`      | choice    | `UDP` or `TCP`; a server on both (4.0.0+) takes either                                                               |
| `pubkey`         | text      | the server's public key, 44 characters (`camex --genkey`)                                                            |
| `psk`            | secret    | the pre-shared key: `hex:`, `base64:` or a passphrase, below 128 bytes                                               |
| `mtu`            | text      | optional, 576 to 9000; empty: the server's                                                                           |
| `keepalive`      | text      | optional, 0 to 3600 seconds; empty: 10                                                                               |
| `server_timeout` | text      | optional, 5 to 3600 seconds without an answer before camex reconnects; empty: 20                                     |
| `web_port`       | text      | optional, 1 to 65535: the port of the app's web page, for nginx on the server (#174); needs a server 3.19.0 or later |

Nothing is stored or logged: the psk is passed to camex and wiped after the
run, and a reason from `check()` names the field, never its value.

### States

| State           | Meaning                                                                                               |
|-----------------|-------------------------------------------------------------------------------------------------------|
| `STOPPED`       | not started, or stopped by `stop()`                                                                   |
| `CONNECTING`    | connecting, or reconnecting after a loss; `lastError()` says why it reconnects                        |
| `CONNECTED`     | the tunnel is up; `address()` is the app's address in it                                              |
| `NEEDS_CONSENT` | no VPN consent (never given, revoked, or another VPN app took over): give it and call `start()` again |
| `FAILED`        | the values were refused (`bad <key>: <reason>`), or the service could not start                       |

`CONNECTED` means camex has a session with its server. When camex loses the
server and reconnects by itself (the VPN stays), the state is `CONNECTING`
(`lastError()`: `no answer from the server, reconnecting`) and turns
`CONNECTED` again once the session is back; the library looks at it every
2 s.

### VPN consent

Android lets an app run a VPN only after a consent. Both ways work at once:

- **A person:** when the user turns the tunnel on, the app calls
  `consentIntent(context)`; when it is not null, the app shows it with
  `startActivityForResult` once. After OK the consent stays, and `start()` works
  from anywhere, on every boot.
- **An unattended device:** the consent is given while the device is
  prepared, over adb, with no dialog and no root:

  ```sh
  adb shell appops set <package> ACTIVATE_VPN allow
  adb shell appops write-settings
  ```

  `write-settings` writes it to disk at once: Android saves app ops later on its
  own, and a reboot before that loses the consent. Once written, it survives
  reboots.

  Android 5.0 (API 21) does not know this operation (`Unknown operation string`):
  there the dialog is confirmed once, on the screen or over adb (`input tap` on
  its OK button), and the consent stays as well.

Without the consent, `start()` returns false and the state is
`NEEDS_CONSENT` (`lastError()` is `VPN consent needed`): nothing is started,
nothing throws, nothing retries by itself. The app shows it (screen,
notification, web page) and calls `start()` again later, as the consent may be
given over adb meanwhile.

### Android 5.0

After `stop()` and a new `start()` in the same process, Android 5.0 sometimes
routes the app's own connections outside the new VPN: its first packets reach
the tunnel with the source address of the device's own network, then they go
out of that network. In our emulator tests that happened in half of the runs,
and it either passed within seconds or not within minutes
([#173](https://github.com/OpenIPC/camex/issues/173), still open). The first
start, new values while running and a server that comes back are not affected;
it was not seen on Android 14.

### Starting without a screen

`start()` works from a `BOOT_COMPLETED` receiver and from the app's own
foreground service once the consent is there. The library stores no values,
so after the process is gone the app starts the tunnel again; a start by the
system without values (an always-on VPN set in the system settings) ends in
`FAILED`.

### The notification

The tunnel runs as a foreground service with an ongoing notification
(`specialUse`, subtype `vpn`). By default it is the library's: title `camex`
(`setNotificationLook`), `Connecting` / `Connected · <address>`, a tap opens
the app. An app that runs its own foreground service passes its own
notification and that service's id, so there is one notification:

```java
CamexTunnel.setNotification(MY_NOTIFICATION_ID,
        (ctx, state, address, error) -> myNotification(state, address));
```

The factory is called on the main thread on each state change; on Android 8
and later the notification's channel is the app's own. When the tunnel stops,
the app's notification stays.

## A link with the settings

`ConnectLink.parse(link)` reads the link `install.sh` prints
([android.md](android.md#a-link-with-the-settings)):
`https://openipc.github.io/camex/connect/#h=&p=&t=&k=&s=` or
`camex://connect?...`. It returns null for any other link, otherwise
`values` (by the keys above: `h` and `p` make `server`, a host without a port
takes 5800, a port without a host gives no `server`), `host` and `port` each on
its own (null when the link has none: an app that keeps them apart puts only
what the link has over its settings, as the camex app does) and `errors` (the
link's malformed fields by their letter, with a short reason). `r` (route all
traffic) is read and ignored. The app puts the values over its own settings,
checks them with `check()` and saves them.

## Tests

- `gradle :tunnel:testReleaseUnitTest` (CI): the fields, the checks, camex's
  command line and the link parser on the JVM.
The instrumented test has its own Activity (it shows the consent dialog on
Android 5.0); it lives in `src/androidTest` only and is not part of the library.

- `tests/android/tunnel-e2e.sh [API]`: the library on an x86_64 emulator in
  Docker. A camex server and a client "peer" run in containers; the
  instrumented test is the second client. It checks `NEEDS_CONSENT` without the
  consent, `FAILED` for bad values, then with the consent (appops; on Android
  5.0 the system dialog, confirmed by a tap): the
  address from the server, the VPN's routes (the server's, one with host bits
  as its network, the Docker network the server is reached through, so camex's
  socket must be protected, and no default route), TCP to the peer through
  the tunnel, TCP from the peer to the app's `0.0.0.0:8080`, `stop()`, and no
  psk in logcat.
