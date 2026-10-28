#!/bin/sh
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# Run as: curl -fsSL https://raw.githubusercontent.com/OpenIPC/camex/master/install.sh | sudo sh
# Docs: https://github.com/OpenIPC/camex/blob/master/docs/install.md
#

# One block: read whole before it runs (a cut download, apt-get reading stdin)
{

steps() {
    cat <<'EOF_STEPS'
camex installer for Debian and Ubuntu. Steps:

  1. Check the host: root, Debian or Ubuntu, systemd, the architecture.
  2. Install the packages it needs with apt-get (curl, ca-certificates;
     with nginx also nginx and openssl).
  3. Download camex of the "latest" release to /usr/local/bin/camex.
  4. Create the server's key pair, /etc/camex/server.key.
  5. Create the server's settings, /etc/camex/camex.conf, with a new
     pre-shared key (psk=) for the clients.
  6. Install the camex service (its arguments in /etc/default/camex),
     enable and start it.
  7. With --domain (or when it was installed before): nginx in front of
     the clients, each one's web UI at http://<client ID>.<domain>/, the
     start page of all of them at http://<domain>/ behind a password.
  8. Print what the clients need: the server's public key and psk.

Run it again at any time: it updates camex, the scripts and the services.
The settings made once (camex.conf, server.key, /etc/default/camex,
nginx's file, the password) are kept as they are; only domain= is added
to camex.conf when nginx comes later.
EOF_STEPS
}

usage() {
    cat >&2 <<'EOF_USAGE'
Usage: install.sh [--domain <domain>] [options]
       install.sh --uninstall

  --domain <domain>       also install nginx in front of the clients
  --port <port>           the server's port (5800)
  --transport udp|tcp     the server's transport (udp)
  --psk <key>             the clients' pre-shared key (a random one)
  --pool <network/prefix> the clients' pool, the server at its first
                          address (10.200.0.0/24)
  --client-timeout <s>    seconds of silence before a client is dropped,
                          60 to 120 (60)
  --public-host <host>    the name or address clients reach this server at,
                          for the printed command and Android link (the
                          domain, else this host's public address)
  With --domain:
  --password <password>   the start page's password, user camex (a random one)
  --http-port <port>      the port nginx listens on (80)
  --upstream-port <port>  the cameras' web port (80)

  The settings are made once: on a rerun a different value is not applied,
  the installer says what to edit.
  --uninstall        list every file of camex on this host and the commands
                     to remove them; removes nothing itself
  -h, --help         this text

Environment (tests, mirrors):
  CAMEX_SRC      the files of examples/ come from $CAMEX_SRC/examples/
                 (https://raw.githubusercontent.com/OpenIPC/camex/master)
  CAMEX_RELEASE  camex-<arch> comes from there
                 (https://github.com/OpenIPC/camex/releases/download/latest)
EOF_USAGE
    exit 2
}

set -u

CAMEX_SRC=${CAMEX_SRC:-https://raw.githubusercontent.com/OpenIPC/camex/master}
CAMEX_RELEASE=${CAMEX_RELEASE:-https://github.com/OpenIPC/camex/releases/download/latest}

BIN=/usr/local/bin/camex
SYNC=/usr/local/bin/camex-nginx-sync
ETC=/etc/camex
CONF=$ETC/camex.conf
KEY=$ETC/server.key
DEFAULTS=/etc/default/camex
UNITS=/etc/systemd/system
NGINX_CONF=/etc/nginx/conf.d/camex-nginx.conf
MAP=/etc/nginx/camex-map.conf
HTPASSWD=/etc/nginx/camex.htpasswd
PASSWORD_FILE=$ETC/nginx-password
PAGE_DIR=/var/lib/camex-nginx
BACKUP=/var/backups/camex
# Where camex-nginx-setup.sh (camex 3.9 and before) put them
OLD_SYNC=/usr/local/bin/camex-nginx-sync.sh
OLD_NGINX_CONF=/etc/nginx/conf.d/camex.conf
# camex 3.10
OLD_TIMER=/etc/systemd/system/camex-nginx-sync.timer

DOMAIN=
PORT=5800
PORT_OPT=
TRANSPORT=
PSK_OPT=
POOL=
CLIENT_TIMEOUT=
PUBLIC_HOST=
PASSWORD=
HTTP_PORT=
UPSTREAM_PORT=
STEPS=8

say() {
    echo "camex-install: $*"
}
die() {
    echo "camex-install: $*" >&2
    exit 1
}
step() {
    echo
    echo "[$1/$STEPS] $2"
}

# fetch <url> <file>: a file:// URL as well (tests)
fetch() {
    curl -fsSL --retry 3 -o "$2" "$1" || die "cannot download $1"
}

# put <file> <destination> <mode>: replaced in one step, the previous one
# kept in $BACKUP when it differs
put() {
    if [ -f "$2" ] && cmp -s "$1" "$2"; then
        chmod "$3" "$2"
        say "unchanged: $2"
        return 1
    fi
    if [ -f "$2" ]; then
        backup "$2"
    fi
    mkdir -p "$(dirname "$2")"
    cp "$1" "$2.new" && chmod "$3" "$2.new" && mv -f "$2.new" "$2" ||
        die "cannot write $2"
    say "installed: $2"
    return 0
}

# backup <file>: a copy in $BACKUP, named after its path and the time
backup() {
    mkdir -p "$BACKUP" && chmod 700 "$BACKUP"
    # mktemp: a name of its own, also for two runs within a second
    b=$(mktemp "$BACKUP/$(echo "$1" | sed 's|^/||; s|/|_|g').$(date +%Y%m%d-%H%M%S).XXXXXX") &&
        cp -p "$1" "$b" || die "cannot keep a copy of $1 in $BACKUP"
    say "previous $1 kept as $b"
}

# conf_server <key> [<file>]: its value in [server] of camex.conf (the last
# one, as camex reads it); not those of [defaults] or [client ID]
conf_server() {
    awk -v k="$1" '/^[[:space:]]*\[/ { s = (tolower($0) ~ /^[[:space:]]*\[[[:space:]]*server[[:space:]]*\]/) }
         s && $0 ~ "^[[:space:]]*" k "[[:space:]]*=" {
             sub(/^[^=]*=[[:space:]]*/, ""); sub(/[[:space:]]+$/, ""); d = $0
         }
         END { if (d != "") print d }' "${2:-$CONF}" 2>/dev/null
}
# urlenc <text>: %XX for every byte but letters, digits and -._~ (the
# Android link's fields, #146)
urlenc() {
    printf '%s' "$1" | od -An -v -tx1 | tr -s ' ' '\n' | grep . | while read -r h; do
        case $h in
            3[0-9]|4[1-9a-f]|5[0-9a]|6[1-9a-f]|7[0-9a]|2d|2e|5f|7e)
                # shellcheck disable=SC2059
                printf "\\$(printf '%03o' "0x$h")" ;;
            *) printf '%%%s' "$h" | tr 'a-f' 'A-F' ;;
        esac
    done
}
# link_psk_ok <psk>: the Android app takes it from a link (ConnectLink: UTF-8,
# no control character, nothing beyond U+FFFF, i.e. no 4-byte UTF-8)
link_psk_ok() {
    # (iconv is libc-bin's; without it the UTF-8 check is left out)
    { ! command -v iconv >/dev/null 2>&1 ||
        printf '%s' "$1" | iconv -f UTF-8 -t UTF-8 >/dev/null 2>&1; } &&
        ! printf '%s' "$1" | LC_ALL=C grep -q "$(printf '[\001-\037\177\360-\377]')"
}
# public_addr: this host's own address towards the internet when it is a
# public one (not behind NAT); none else
public_addr() {
    a=$(ip -4 route get 1.1.1.1 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p' | head -n 1)
    case "$a" in
        # private, shared (CGNAT), loopback, link-local, documentation,
        # benchmarking, multicast and reserved: none is reachable from outside
        ''|0.*|10.*|127.*|169.254.*|192.168.*|100.6[4-9].*|100.[7-9][0-9].*|100.1[01][0-9].*|100.12[0-7].*|172.1[6-9].*|172.2[0-9].*|172.3[01].*) ;;
        192.0.0.*|192.0.2.*|198.51.100.*|203.0.113.*|198.1[89].*|22[4-9].*|2[3-5][0-9].*) ;;
        *) printf '%s\n' "$a" ;;
    esac
}
# read_args: what camex.service really runs with, from CAMEX_ARGS of
# /etc/default/camex as the shell splits it there; its command line wins over
# camex.conf (an older unit's arguments moved there may have --psk or --port)
read_args() {
    ARGS=$(sed -n 's/^CAMEX_ARGS="\{0,1\}\([^"]*\)"\{0,1\}$/\1/p' "$DEFAULTS" | tail -n 1)
    # camex reads /etc/camex/camex.conf without --config
    USED_CONF=$CONF; USED_KEY=; ARG_PSK=; ARG_PORT=; PROTO=udp
    set -f
    # shellcheck disable=SC2086
    set -- $ARGS
    set +f
    while [ $# -gt 0 ]; do
        case "$1" in
            --config|-f)       USED_CONF=${2-}; shift ;;
            --config=*)        USED_CONF=${1#*=} ;;
            --server-key-file|-S) USED_KEY=${2-}; shift ;;
            --server-key-file=*)  USED_KEY=${1#*=} ;;
            --psk|-k)          ARG_PSK=${2-}; shift ;;
            --psk=*)           ARG_PSK=${1#*=} ;;
            --port|-p)         ARG_PORT=${2-}; shift ;;
            --port=*)          ARG_PORT=${1#*=} ;;
            --transport|-R)    PROTO=${2-}; shift ;;
            --transport=*)     PROTO=${1#*=} ;;
            -R*)               PROTO=${1#-R} ;;
            -f*)               USED_CONF=${1#-f} ;;
            -S*)               USED_KEY=${1#-S} ;;
            -k*)               ARG_PSK=${1#-k} ;;
            -p*)               ARG_PORT=${1#-p} ;;
        esac
        shift
    done
}
conf_domain() {
    conf_server domain | tr 'A-Z' 'a-z'
}

# --uninstall: every file of camex, and the commands to remove them
uninstall_list() {
    echo "Files of camex on this host (+ there, - not there)."
    echo "This removes nothing: check the list and remove what you want."
    echo
    found=
    for f in \
        "$BIN" "$SYNC" "$OLD_SYNC" \
        "$UNITS/camex.service" "$UNITS/camex-nginx-sync.service" \
        "$OLD_TIMER" \
        "$DEFAULTS" "$CONF" "$CONF.bak" "$KEY" "$PASSWORD_FILE" \
        "$NGINX_CONF" "$OLD_NGINX_CONF" "$HTPASSWD" \
        "$MAP" "$MAP.applied" "$MAP.lock" "$MAP.warnings" \
        "$MAP.warnings-index" \
        "$PAGE_DIR/index.html" "$PAGE_DIR" /run/camex "$BACKUP"; do
        if [ -e "$f" ]; then
            echo "  + $f"
            found="$found $f"
        else
            echo "  - $f"
        fi
    done
    echo
    echo "To remove camex, run as root:"
    echo
    # Only units that are there: systemctl stops at one it does not know
    units=
    for u in camex-nginx-sync.timer camex-nginx-sync.service camex.service; do
        [ -f "$UNITS/$u" ] && units="$units $u"
    done
    [ -n "$units" ] && echo "  systemctl disable --now$units"
    for f in $found; do
        case "$f" in
            "$ETC"/*|"$BACKUP"|/run/camex) ;;
            "$PAGE_DIR") echo "  rm -r $f" ;;
            "$PAGE_DIR"/*) ;;   # with its directory
            *) echo "  rm $f" ;;
        esac
    done
    echo "  systemctl daemon-reload"
    if echo "$found" | grep -qF /etc/nginx/; then
        echo "  nginx -t && systemctl reload nginx"
    fi
    echo
    echo "Then, only if you no longer need them:"
    echo
    echo "  rm -r $ETC   # the server's key: the clients would need a new one"
    [ -e "$BACKUP" ] && echo "  rm -r $BACKUP   # copies of replaced files"
    echo "  apt-get remove nginx   # if camex was its only user"
    exit 0
}

while [ $# -gt 0 ]; do
    # An option given with an empty value is a mistake, not "the default"
    case "$1" in
        --domain|--port|--transport|--psk|--pool|--password|--http-port|--upstream-port|--client-timeout|--public-host)
            [ $# -ge 2 ] || usage
            [ -n "$2" ] || die "$1: empty" ;;
    esac
    case "$1" in
        --domain)    [ $# -ge 2 ] || usage; DOMAIN=$2; shift 2 ;;
        --port)      [ $# -ge 2 ] || usage; PORT=$2; PORT_OPT=$2; shift 2 ;;
        --transport) [ $# -ge 2 ] || usage; TRANSPORT=$2; shift 2 ;;
        --psk)       [ $# -ge 2 ] || usage; PSK_OPT=$2; shift 2 ;;
        --pool)      [ $# -ge 2 ] || usage; POOL=$2; shift 2 ;;
        --client-timeout) [ $# -ge 2 ] || usage; CLIENT_TIMEOUT=$2; shift 2 ;;
        --public-host) [ $# -ge 2 ] || usage; PUBLIC_HOST=$2; shift 2 ;;
        --password)  [ $# -ge 2 ] || usage; PASSWORD=$2; shift 2 ;;
        --http-port) [ $# -ge 2 ] || usage; HTTP_PORT=$2; shift 2 ;;
        --upstream-port) [ $# -ge 2 ] || usage; UPSTREAM_PORT=$2; shift 2 ;;
        --uninstall) uninstall_list ;;
        -h|--help)   usage ;;
        *)           echo "camex-install: unknown option $1" >&2; usage ;;
    esac
done

# check_domain: --domain in lower case, as camex checks it: labels of
# letters, digits and '-', 253 characters
check_domain() {
    DOMAIN=$(printf '%s' "$DOMAIN" | tr 'A-Z' 'a-z')
    if [ -n "$DOMAIN" ] && { [ ${#DOMAIN} -gt 253 ] || ! printf '%s\n' "$DOMAIN" |
       grep -qxE '[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?(\.[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?)*'; }; then
        die "not a domain: $DOMAIN_GIVEN"
    fi
}
# idna_domain: --domain in another script as its IDNA form (xn--...), the
# one DNS, nginx and camex use; idn2 makes it (installed for it)
idna_domain() {
    if ! command -v idn2 >/dev/null 2>&1; then
        say "apt-get install idn2 (for --domain $DOMAIN)"
        { DEBIAN_FRONTEND=noninteractive apt-get update -qq &&
          DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends idn2
        } </dev/null >/dev/null || die "apt-get install idn2 failed"
    fi
    # (under sudo the locale is often C, where idn2 reads no UTF-8)
    idn=$(LC_ALL=C.UTF-8 idn2 --quiet -- "$DOMAIN" 2>/dev/null) && [ -n "$idn" ] ||
        die "not a domain: $DOMAIN (idn2 cannot make its IDNA form)"
    say "--domain $DOMAIN is $idn (its IDNA form, used everywhere)"
    DOMAIN=$idn
    check_domain
}
# A domain in another script (мойдомен.рф, #151) is turned into its IDNA
# form in step 1, after the host is checked (it may need idn2 installed)
DOMAIN_GIVEN=$DOMAIN
case "$DOMAIN" in
    *[![:print:]]*|*[!\ -~]*) DOMAIN_IDN=1 ;;
    *) DOMAIN_IDN=; check_domain ;;
esac
check_port() {   # check_port <option> <value>
    case "$2" in
        ''|*[!0-9]*) die "$1: not a port: $2" ;;
    esac
    [ "$2" -ge 1 ] && [ "$2" -le 65535 ] || die "$1: not a port: $2"
}
check_port --port "$PORT"
[ -n "$HTTP_PORT" ] && check_port --http-port "$HTTP_PORT"
[ -n "$UPSTREAM_PORT" ] && check_port --upstream-port "$UPSTREAM_PORT"
# As camex takes it (#143)
case "$CLIENT_TIMEOUT" in
    '') ;;
    *[!0-9]*) die "--client-timeout: 60 to 120 seconds, not $CLIENT_TIMEOUT" ;;
    *) [ "$CLIENT_TIMEOUT" -ge 60 ] && [ "$CLIENT_TIMEOUT" -le 120 ] ||
           die "--client-timeout: 60 to 120 seconds, not $CLIENT_TIMEOUT"
       # 060 is 60, as camex reads it
       CLIENT_TIMEOUT=$(printf '%s' "$CLIENT_TIMEOUT" | sed 's/^0*//') ;;
esac
case "${TRANSPORT:-udp}" in
    udp|tcp) ;;
    *) die "--transport: udp or tcp, not $TRANSPORT" ;;
esac
# As camex reads them: hex:<64 hex digits>, base64:<44 characters>, both
# 32 bytes and not all zero, or a passphrase; one line, below 128
# characters, no blank at its ends (camex trims those off)
if [ -n "$PSK_OPT" ]; then
    case "$PSK_OPT" in
        *"
"*) die "--psk: one line" ;;
        [[:space:]]*|*[[:space:]]) die "--psk: no blank at its start or end" ;;
    esac
    # In bytes, as camex counts it (a UTF-8 letter may be two or more)
    [ "$(printf '%s' "$PSK_OPT" | LC_ALL=C wc -c)" -lt 128 ] || die "--psk: below 128 bytes"
    case "$PSK_OPT" in
        hex:*)
            printf '%s\n' "${PSK_OPT#hex:}" | grep -qxE '[0-9a-fA-F]{64}' ||
                die "--psk: hex: needs 64 hex digits"
            printf '%s\n' "${PSK_OPT#hex:}" | grep -qxE '0{64}' &&
                die "--psk: an all-zero key" ;;
        base64:*)
            b=${PSK_OPT#base64:}
            printf '%s\n' "$b" | grep -qxE '[A-Za-z0-9+/]{43}=' ||
                die "--psk: base64: needs 44 characters (32 bytes)"
            raw=$(printf '%s' "$b" | base64 -d 2>/dev/null | od -An -tx1 | tr -d ' \n')
            [ ${#raw} -eq 64 ] || die "--psk: base64: not 32 bytes"
            [ "$raw" != "$(printf '%064d' 0)" ] || die "--psk: an all-zero key" ;;
    esac
fi
# A host name or an IPv4 address (camex resolves IPv4 only), in camex's
# server_host[64]
if [ -n "$PUBLIC_HOST" ]; then
    [ ${#PUBLIC_HOST} -le 63 ] && printf '%s\n' "$PUBLIC_HOST" |
        grep -qxE '[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?(\.[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*' ||
        die "--public-host: a host name or an IPv4 address, up to 63 characters"
fi
case "$PASSWORD" in
    *"
"*) die "--password: one line" ;;
esac
# The pool as camex takes it (/8 to /30), its network and the server's
# address, the first one: "<network>/<prefix> <first>/<prefix>"
if [ -n "$POOL" ]; then
    POOL_NET=$(printf '%s\n' "$POOL" | awk -F'[./]' 'NF == 5 {
        for (i = 1; i <= 4; i++) if ($i !~ /^[0-9]+$/ || $i > 255) exit 1
        p = $5; if (p !~ /^[0-9]+$/ || p < 8 || p > 30) exit 1
        n = (($1 * 256 + $2) * 256 + $3) * 256 + $4
        s = 2 ^ (32 - p); net = n - n % s; f = net + 1
        printf "%d.%d.%d.%d/%d %d.%d.%d.%d/%d\n",
            int(net / 16777216) % 256, int(net / 65536) % 256,
            int(net / 256) % 256, net % 256, p,
            int(f / 16777216) % 256, int(f / 65536) % 256,
            int(f / 256) % 256, f % 256, p
        ok = 1; exit 0 }
        END { if (!ok) exit 1 }') ||
        die "--pool: <network>/<prefix>, /8 to /30: $POOL"
    POOL_FIRST=${POOL_NET#* }
    POOL_NET=${POOL_NET% *}
fi
if [ -z "$DOMAIN" ] && [ -n "$PASSWORD$HTTP_PORT$UPSTREAM_PORT" ]; then
    die "--password, --http-port and --upstream-port are for nginx: give --domain"
fi

steps

TMP=$(mktemp -d) || die "cannot make a temporary directory"
trap 'rm -rf "$TMP"' EXIT
trap 'exit 130' INT TERM HUP

# 1. The host ---------------------------------------------------------------
step 1 "Checking the host"
[ "$(id -u)" = 0 ] || die "run it as root (sudo sh)"
# (a missing file would end sh at ".": checked first)
[ -r /etc/os-release ] || die "no /etc/os-release: not Debian or Ubuntu"
. /etc/os-release
case " ${ID-} ${ID_LIKE-} " in
    *" debian "*|*" ubuntu "*) ;;
    *) die "this is ${PRETTY_NAME:-an unknown system}, not Debian or Ubuntu:" \
           "install camex by hand:" \
           "https://github.com/OpenIPC/camex/blob/master/docs/install.md#by-hand" ;;
esac
command -v apt-get >/dev/null 2>&1 || die "no apt-get: not Debian or Ubuntu"
[ -d /run/systemd/system ] || die "systemd is not running: install camex by hand"
case "$(dpkg --print-architecture)" in
    amd64)        ARCH=x86_64 ;;
    arm64)        ARCH=aarch64 ;;
    armhf|armel)  ARCH=arm ;;
    mipsel)       ARCH=mipsel ;;
    *) die "no camex build for $(dpkg --print-architecture): build it from source:" \
       "https://github.com/OpenIPC/camex/blob/master/docs/build.md" ;;
esac
[ -n "$DOMAIN_IDN" ] && idna_domain
# nginx installed before: a rerun keeps it up to date
if [ -z "$DOMAIN" ] && { [ -f "$NGINX_CONF" ] || [ -f "$OLD_NGINX_CONF" ]; }; then
    [ -f "$CONF" ] && DOMAIN=$(conf_domain)
    [ -n "$DOMAIN" ] || die "nginx's file of camex is there, but no domain= in" \
                            "$CONF: run with --domain <domain>"
fi
# The domain is never changed: it is in camex.conf, nginx and DNS
if [ -n "$DOMAIN" ] && [ -f "$CONF" ]; then
    have=$(conf_domain)
    if [ -n "$have" ] && [ "$have" != "$DOMAIN" ]; then
        die "$CONF has domain=$have, not $DOMAIN. To change the domain, edit" \
            "domain= there and the names in $NGINX_CONF yourself"
    fi
fi
# nginx's files: what would stop step 7 stops here, before camex.conf gets
# the domain
if [ -n "$DOMAIN" ]; then
    if [ -f "$OLD_NGINX_CONF" ] && [ -f "$NGINX_CONF" ]; then
        die "both $OLD_NGINX_CONF and $NGINX_CONF are there: move what you" \
            "need into $NGINX_CONF, remove the other, then run this again"
    fi
    kept=$NGINX_CONF
    [ -f "$kept" ] || kept=$OLD_NGINX_CONF
    if [ -f "$kept" ] &&
       ! grep -qiE "server_name[[:space:]]+\*\.$(echo "$DOMAIN" | sed 's/\./\\./g');" "$kept"; then
        die "$kept is not for $DOMAIN: fix its server_name lines (or remove" \
            "it), then run this again"
    fi
    # Another file of nginx with the map: two of them would fight
    for f in /etc/nginx/conf.d/*.conf /etc/nginx/sites-enabled/*; do
        [ -f "$f" ] && [ "$f" != "$NGINX_CONF" ] && [ "$f" != "$OLD_NGINX_CONF" ] &&
            grep -qE "^[[:space:]]*include[[:space:]]+$MAP;" "$f" &&
            die "$f is a camex file of nginx too: move what you need from it" \
                "into $NGINX_CONF (or remove it), then run this again"
    done
fi
say "${PRETTY_NAME:-Debian}, camex-$ARCH${DOMAIN:+, nginx for $DOMAIN}"

# 2. Packages ---------------------------------------------------------------
step 2 "Installing packages"
PKGS="ca-certificates curl"
[ -n "$DOMAIN" ] && PKGS="$PKGS nginx openssl"
missing=
for p in $PKGS; do
    dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q 'ok installed' ||
        missing="$missing $p"
done
if [ -n "$missing" ]; then
    say "apt-get install$missing"
    DEBIAN_FRONTEND=noninteractive apt-get update -qq </dev/null ||
        die "apt-get update failed"
    # shellcheck disable=SC2086
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
        $missing </dev/null >/dev/null || die "apt-get install$missing failed"
else
    say "all there: $PKGS"
fi

# 3. camex ------------------------------------------------------------------
step 3 "Downloading camex ($CAMEX_RELEASE/camex-$ARCH)"
fetch "$CAMEX_RELEASE/camex-$ARCH" "$TMP/camex"
# Tried next to its place: /tmp may be mounted noexec
mkdir -p "$(dirname "$BIN")"
cp "$TMP/camex" "$BIN.try" && chmod 755 "$BIN.try" || die "cannot write $BIN.try"
if ! "$BIN.try" --version >/dev/null 2>&1; then
    rm -f "$BIN.try"
    die "the downloaded camex-$ARCH does not run here"
fi
rm -f "$BIN.try"
NEW_BIN=
put "$TMP/camex" "$BIN" 755 && NEW_BIN=1
say "$("$BIN" --version | head -n 1)"

# 4. The key pair -----------------------------------------------------------
step 4 "The server's key pair: $KEY"
mkdir -p "$ETC" && chmod 700 "$ETC"
if [ -s "$KEY" ]; then
    say "kept: $KEY"
else
    (umask 077; "$BIN" --genkey > "$KEY.new") && mv -f "$KEY.new" "$KEY" ||
        die "cannot make $KEY"
    say "created: $KEY"
fi
PUBKEY=$(sed -n 's/^public_key=//p' "$KEY")

# 5. camex.conf -------------------------------------------------------------
step 5 "The server's settings: $CONF"
if [ -f "$CONF" ]; then
    say "kept: $CONF"
    # nginx added later: its domain goes to [server]
    if [ -n "$DOMAIN" ] && [ -z "$(conf_domain)" ]; then
        backup "$CONF"
        awk -v d="$DOMAIN" '
            !done && tolower($0) ~ /^[[:space:]]*\[[[:space:]]*server[[:space:]]*\]/ {
                print; print "domain=" d; done = 1; next
            }
            { print }
            END { if (!done) { print ""; print "[server]"; print "domain=" d } }' \
            "$CONF" > "$TMP/camex.conf" &&
            cat "$TMP/camex.conf" > "$CONF" || die "cannot write $CONF"
        say "added domain=$DOMAIN to [server]"
    fi
else
    if [ -n "$PSK_OPT" ]; then
        PSK=$PSK_OPT
    else
        PSK=hex:$(od -An -tx1 -N32 /dev/urandom | tr -d ' \n')
        [ ${#PSK} -eq 68 ] || die "cannot make a pre-shared key"
    fi
    {
        echo "# camex server settings, made by install.sh; see"
        echo "# https://github.com/OpenIPC/camex/blob/master/examples/camex.conf"
        echo "[server]"
        echo "port=$PORT"
        echo "# The server's address in the tunnel and the clients' pool"
        echo "local_cidr=${POOL_FIRST:-10.200.0.1/24}"
        echo "auto_pool=${POOL_NET:-10.200.0.0/24}"
        echo "# Clients do not reach each other"
        echo "client_isolation=yes"
        echo "# Seconds of silence before a client is dropped (the start page's"
        echo "# lamp turns red); clients before camex 3.13 with --keepalive 0 or"
        echo "# above 20 need 120"
        echo "client_timeout=${CLIENT_TIMEOUT:-60}"
        echo "# The clients' pre-shared key"
        printf 'psk=%s\n' "$PSK"
        [ -n "$DOMAIN" ] && echo "domain=$DOMAIN"
    } > "$TMP/camex.conf"
    (umask 077; cp "$TMP/camex.conf" "$CONF") || die "cannot write $CONF"
    chmod 600 "$CONF"
    say "created: $CONF (port $PORT, pool ${POOL_NET:-10.200.0.0/24}," \
        "${PSK_OPT:+the given psk}${PSK_OPT:-a new psk})"
fi
# Settings made once: a different value is said, not applied
if [ -n "$PORT_OPT" ] && [ "$(conf_server port)" != "$PORT_OPT" ]; then
    say "kept port= of $CONF, not --port $PORT_OPT: edit it there, then" \
        "systemctl restart camex (the clients need the same)"
fi
if [ -n "$PSK_OPT" ] && [ "$(conf_server psk)" != "$PSK_OPT" ]; then
    say "kept psk= of $CONF, not --psk: edit it there (the clients need the same)"
fi
# Without the key camex uses 120 s, the same as --client-timeout 120
have=$(conf_server client_timeout | sed 's/^0*\([0-9]\)/\1/')
if [ -n "$CLIENT_TIMEOUT" ] && [ "${have:-120}" != "$CLIENT_TIMEOUT" ]; then
    say "kept client_timeout= of $CONF, not --client-timeout $CLIENT_TIMEOUT:" \
        "edit it there, then systemctl reload camex"
fi
if [ -n "$POOL" ] && [ "$(conf_server auto_pool)" != "$POOL_NET" ]; then
    say "kept auto_pool= of $CONF, not --pool $POOL_NET: edit auto_pool= and" \
        "local_cidr= there, then systemctl reload camex"
fi

# 6. The service ------------------------------------------------------------
step 6 "The camex service"
if [ ! -f "$DEFAULTS" ]; then
    # The arguments of a unit made by hand (camex 3.9 and before) stay
    args=$(sed -n 's/^Environment="\{0,1\}CAMEX_ARGS=\(.*\)/\1/p' \
           "$UNITS/camex.service" 2>/dev/null | sed 's/"$//' | tail -n 1)
    case "$args" in
        ''|*SERVER_PUBLIC_KEY*)
            args="--mode server --config $CONF --server-key-file $KEY"
            [ "$TRANSPORT" = tcp ] && args="$args --transport tcp" ;;
    esac
    {
        echo "# The arguments of camex.service; systemctl restart camex after a change"
        echo "CAMEX_ARGS=\"$args\""
    } > "$TMP/defaults"
    # 0600: an older unit's arguments may carry --psk
    (umask 077; cp "$TMP/defaults" "$DEFAULTS") && chmod 600 "$DEFAULTS" ||
        die "cannot write $DEFAULTS"
    say "created: $DEFAULTS"
else
    say "kept: $DEFAULTS"
fi
# Also when an older unit's arguments were moved there
if [ -n "$TRANSPORT" ]; then
    read_args
    have=udp
    [ "$PROTO" = tcp ] && have=tcp
    [ "$have" = "$TRANSPORT" ] ||
        say "kept the server's transport $have, not --transport $TRANSPORT:" \
            "edit CAMEX_ARGS in $DEFAULTS, then systemctl restart camex"
fi
fetch "$CAMEX_SRC/examples/camex.service" "$TMP/camex.service"
NEW_UNIT=
put "$TMP/camex.service" "$UNITS/camex.service" 644 && NEW_UNIT=1
systemctl daemon-reload
systemctl enable camex.service >/dev/null 2>&1 || die "cannot enable camex.service"
if ! systemctl is-active --quiet camex.service; then
    systemctl start camex.service || die "camex did not start: journalctl -u camex"
    say "started camex.service"
elif [ -n "$NEW_BIN$NEW_UNIT" ]; then
    systemctl restart camex.service || die "camex did not restart: journalctl -u camex"
    say "restarted camex.service"
else
    # camex.conf may have a new domain
    systemctl reload camex.service ||
        die "systemctl reload camex failed: journalctl -u camex"
    say "reloaded camex.service (its log says whether camex.conf was taken)"
fi
sleep 1
systemctl is-active --quiet camex.service ||
    die "camex.service is not running: journalctl -u camex"

# 7. nginx ------------------------------------------------------------------
step 7 "nginx in front of the clients"
if [ -z "$DOMAIN" ]; then
    say "skipped: run again with --domain <domain> to install it"
else
    # camex-nginx-setup.sh's names (camex 3.9 and before) become the new ones
    if [ -f "$OLD_NGINX_CONF" ] && [ ! -f "$NGINX_CONF" ]; then
        mv "$OLD_NGINX_CONF" "$NGINX_CONF" || die "cannot rename $OLD_NGINX_CONF"
        say "renamed $OLD_NGINX_CONF to $NGINX_CONF"
    fi
    # (its server_name and the other files were checked in step 1)
    if [ -f "$NGINX_CONF" ]; then
        say "kept: $NGINX_CONF"
        # Its port is what the links and --check use (#141): that of the
        # first plain HTTP listen line (<port>, <address>:<port>, [::]:<port>;
        # not ssl or quic)
        NGINX_PORT=$(awk '/^[[:space:]]*listen[[:space:]]/ &&
                          !/[[:space:]](ssl|quic)([[:space:];]|$)/ {
                              v = $2; sub(/;.*$/, "", v); sub(/^.*:/, "", v)
                              if (v ~ /^[0-9]+$/) { print v; exit }
                          }' "$NGINX_CONF")
        [ -z "$HTTP_PORT" ] || [ "$HTTP_PORT" = "${NGINX_PORT:-80}" ] ||
            say "kept the listen lines of $NGINX_CONF, not --http-port $HTTP_PORT:" \
                "edit them there, then run this again"
        grep -qiE "server_name[[:space:]]+$(echo "$DOMAIN" | sed 's/\./\\./g');" "$NGINX_CONF" ||
            say "WARNING: $NGINX_CONF has no server block for $DOMAIN itself:" \
                "no start page there (see examples/nginx-camex.conf)"
        # A file of its own (or an older one) may serve the start page
        # without the password
        grep -qE '^[[:space:]]*auth_basic[[:space:]]+"' "$NGINX_CONF" ||
            say "WARNING: $NGINX_CONF has no auth_basic: the start page" \
                "has no password (see auth_basic in examples/nginx-camex.conf)"
    else
        fetch "$CAMEX_SRC/examples/nginx-camex.conf" "$TMP/nginx.conf"
        sed -i "s/openipc\.click/$DOMAIN/g" "$TMP/nginx.conf"
        [ -n "$HTTP_PORT" ] &&
            sed -i "s/^\([[:space:]]*listen[[:space:]]\{1,\}\)80;/\1$HTTP_PORT;/" "$TMP/nginx.conf"
        put "$TMP/nginx.conf" "$NGINX_CONF" 644
        NGINX_PORT=${HTTP_PORT:-80}
    fi
    if [ ! -f "$MAP" ]; then
        printf 'map $host $camex_upstream {\n}\n' > "$MAP"
        say "created: $MAP (empty)"
    fi
    # The start page's password: user camex, made once
    pw_new=
    if [ ! -s "$PASSWORD_FILE" ]; then
        if [ -n "$PASSWORD" ]; then
            (umask 077; printf '%s\n' "$PASSWORD" > "$PASSWORD_FILE")
        else
            (umask 077; openssl rand -hex 10 > "$PASSWORD_FILE")
        fi || die "cannot make the password"
        pw_new=1
        say "created: $PASSWORD_FILE"
    fi
    if [ -n "$PASSWORD" ] && [ -z "$pw_new" ] &&
       [ "$(head -n 1 "$PASSWORD_FILE")" != "$PASSWORD" ]; then
        say "kept the start page's password, not --password: remove" \
            "$PASSWORD_FILE and run this again to change it"
    fi
    # Its line in nginx's file; the other users there stay
    if [ -n "$pw_new" ] || ! grep -q '^camex:' "$HTPASSWD" 2>/dev/null; then
        hash=$(openssl passwd -apr1 -stdin < "$PASSWORD_FILE") ||
            die "cannot hash the password"
        { grep -v '^camex:' "$HTPASSWD" 2>/dev/null; echo "camex:$hash"; } \
            > "$TMP/htpasswd"
        cp "$TMP/htpasswd" "$HTPASSWD.new" && chgrp www-data "$HTPASSWD.new" &&
            chmod 640 "$HTPASSWD.new" && mv -f "$HTPASSWD.new" "$HTPASSWD" ||
            die "cannot write $HTPASSWD for nginx (group www-data)"
        say "wrote the camex line of $HTPASSWD"
    fi
    # The sync script and its service
    fetch "$CAMEX_SRC/examples/camex-nginx-sync.sh" "$TMP/sync"
    put "$TMP/sync" "$SYNC" 755 || true
    if [ -f "$OLD_SYNC" ]; then
        backup "$OLD_SYNC"
        rm -f "$OLD_SYNC"
        say "removed $OLD_SYNC (now $SYNC)"
    fi
    fetch "$CAMEX_SRC/examples/camex-nginx-sync.service" "$TMP/sync.service"
    # The start page where the installed service has it (camex-nginx-setup.sh
    # --index, an edit; empty: none), as nginx's kept file serves it
    if grep -q '^Environment=INDEX=' "$UNITS/camex-nginx-sync.service" 2>/dev/null; then
        index=$(sed -n 's/^Environment=INDEX=//p' "$UNITS/camex-nginx-sync.service" | tail -n 1)
        case "$index" in
            *[!A-Za-z0-9/._-]*) say "kept no INDEX=$index: not a plain path" ;;
            *) sed -i "s|^Environment=INDEX=.*|Environment=INDEX=$index|" "$TMP/sync.service" ;;
        esac
    fi
    # Its ports (#141): HTTP_PORT that of nginx's file (kept or just made);
    # UPSTREAM_PORT made once: that of the installed service (none there:
    # 80), else the option
    for v in HTTP_PORT UPSTREAM_PORT; do
        eval "opt=\$$v"
        if [ $v = HTTP_PORT ]; then
            val=${NGINX_PORT:-80}
        elif [ -f "$UNITS/camex-nginx-sync.service" ]; then
            val=$(sed -n "s/^Environment=$v=\([0-9]*\)\$/\1/p" \
                  "$UNITS/camex-nginx-sync.service" | tail -n 1)
            val=${val:-80}
            [ -z "$opt" ] || [ "$opt" = "$val" ] ||
                say "kept $v=$val of camex-nginx-sync.service, not $opt: edit it" \
                    "in $UNITS/camex-nginx-sync.service"
        else
            val=${opt:-80}
        fi
        [ "$val" != 80 ] &&
            sed -i "/^Environment=INDEX=/a Environment=$v=$val" "$TMP/sync.service"
        [ "$v" = HTTP_PORT ] && WEB_PORT=$val
    done
    put "$TMP/sync.service" "$UNITS/camex-nginx-sync.service" 644 || true
    # camex 3.10's timer: the service follows the servers itself now
    if [ -f "$OLD_TIMER" ]; then
        systemctl disable --now camex-nginx-sync.timer >/dev/null 2>&1
        backup "$OLD_TIMER"
        rm -f "$OLD_TIMER"
        say "removed $OLD_TIMER (camex-nginx-sync.service follows the servers)"
    fi
    systemctl daemon-reload
    nginx -t 2>"$TMP/nginx-t" || { cat "$TMP/nginx-t" >&2; die "nginx -t rejects the configuration"; }
    if systemctl is-active --quiet nginx; then
        systemctl reload nginx || die "systemctl reload nginx failed"
    else
        systemctl enable --now nginx >/dev/null 2>&1 || die "nginx did not start"
    fi
    # Restarted: a new script or unit takes over
    systemctl enable camex-nginx-sync.service >/dev/null 2>&1 &&
        systemctl restart camex-nginx-sync.service ||
        die "cannot start camex-nginx-sync.service: journalctl -u camex-nginx-sync"
    sleep 2   # its first sync
    say "checking: $SYNC --check"
    "$SYNC" --check || say "see the FAIL lines above (DNS is yours to set up)"
fi

# 8. The result -------------------------------------------------------------
step 8 "Done"
# What camex really uses: its command line wins over camex.conf (an older
# unit's arguments moved to /etc/default/camex may have --psk or --port)
read_args
if [ "$USED_CONF" != "$CONF" ]; then
    say "WARNING: camex.service reads $USED_CONF, not $CONF" \
        "(CAMEX_ARGS in $DEFAULTS): the settings made here are not used"
fi
if [ -n "$USED_KEY" ]; then
    [ "$USED_KEY" != "$KEY" ] && [ -r "$USED_KEY" ] &&
        PUBKEY=$(sed -n 's/^public_key=//p' "$USED_KEY")
elif [ -n "$(conf_server server_key "$USED_CONF")" ]; then
    # Its key is server_key= of camex.conf, whose public key is not kept
    PUBKEY="<the public key of server_key= in $USED_CONF>"
    say "WARNING: camex uses server_key= of $USED_CONF, not $KEY: give the" \
        "clients the public key made with it"
fi
PSK=${ARG_PSK:-$(conf_server psk "$USED_CONF")}
PORT_NOW=${ARG_PORT:-$(conf_server port "$USED_CONF")}
# Quoted for the shell the command is pasted into: ' as '\''
PSK_Q=$(printf '%s' "${PSK:-<psk= of $USED_CONF>}" | sed "s/'/'\\\\''/g")
echo
echo "camex server is running: systemctl status camex, camex --status"
TOPT=
[ "$PROTO" = tcp ] && TOPT=" --transport tcp" || PROTO=udp
echo "Open $(echo $PROTO | tr a-z A-Z) port ${PORT_NOW:-5800} in the firewall${DOMAIN:+, and TCP ${WEB_PORT:-80} for nginx}."
# Where the clients reach this server: --public-host, the domain of the
# camex.conf the service reads (its DNS points here; a rerun keeps it), this
# host's public address
HOST=${PUBLIC_HOST:-$(conf_server domain "$USED_CONF" | tr 'A-Z' 'a-z')}
HOST=${HOST:-$(public_addr)}
echo "A client connects with:"
echo
echo "  camex --mode client --auto --server-host ${HOST:-<this host>} --port ${PORT_NOW:-5800}$TOPT \\"
printf '%s\n' "      --server-pubkey $PUBKEY --psk '$PSK_Q'"
# The same settings for the Android app (#146): the fields after '#' never
# leave the phone's browser; the psk is in it, so it is a password
case "$PUBKEY" in
    "<"*) ;;
    *)
        if [ ${#HOST} -gt 63 ]; then
            echo
            echo "Android: no link, the host $HOST is longer than the app takes (63):"
            echo "run this again with a shorter --public-host."
        elif [ -n "$PSK" ] && ! link_psk_ok "$PSK"; then
            echo
            echo "Android: no link, the psk has a character the app does not take (a"
            echo "control character or one beyond U+FFFF): type it in the app's settings."
        elif [ -n "$PSK" ]; then
            echo
            echo "Android: open this link on the phone (Telegram, mail, anything);"
            echo "it has the psk, send it only to the device's owner:"
            printf '%s\n' "  https://openipc.github.io/camex/connect/#h=$(urlenc "${HOST:-this-host}")&p=${PORT_NOW:-5800}&t=$PROTO&k=$(urlenc "$PUBKEY")&s=$(urlenc "$PSK")"
            [ -n "$HOST" ] || echo "  (put this server's name or address after h=, or run again with --public-host)"
        fi ;;
esac
if [ -n "$DOMAIN" ]; then
    WP=
    [ "${WEB_PORT:-80}" != 80 ] && WP=":$WEB_PORT"
    echo
    echo "Clients' web UIs: http://<client ID>.$DOMAIN$WP/"
    printf '%s\n' "Start page:       http://$DOMAIN$WP/  (user camex, password $(head -n 1 "$PASSWORD_FILE"))"
    echo "DNS records *.$DOMAIN and $DOMAIN must point to this host (docs/nginx.md)."
fi
echo
echo "Files and how to remove them: install.sh --uninstall"

exit 0
}
