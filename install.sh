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
Usage: install.sh [--domain <domain>] [--port <port>]
       install.sh --uninstall

  --domain <domain>  also install nginx in front of the clients for <domain>
  --port <port>      the server's UDP port in a new camex.conf (5800)
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

DOMAIN=
PORT=5800
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
        "$UNITS/camex-nginx-sync.timer" \
        "$DEFAULTS" "$CONF" "$CONF.bak" "$KEY" "$PASSWORD_FILE" \
        "$NGINX_CONF" "$OLD_NGINX_CONF" "$HTPASSWD" \
        "$MAP" "$MAP.applied" "$MAP.lock" "$MAP.warnings" \
        "$MAP.warnings-index" \
        "$PAGE_DIR" /run/camex "$BACKUP"; do
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
    case "$1" in
        --domain)    [ $# -ge 2 ] || usage; DOMAIN=$2; shift 2 ;;
        --port)      [ $# -ge 2 ] || usage; PORT=$2; shift 2 ;;
        --uninstall) uninstall_list ;;
        -h|--help)   usage ;;
        *)           echo "camex-install: unknown option $1" >&2; usage ;;
    esac
done

DOMAIN=$(printf '%s' "$DOMAIN" | tr 'A-Z' 'a-z')
# As camex checks it: labels of letters, digits and '-', 253 characters
if [ -n "$DOMAIN" ] && { [ ${#DOMAIN} -gt 253 ] || ! printf '%s\n' "$DOMAIN" |
   grep -qxE '[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?(\.[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?)*'; }; then
    die "not a domain: $DOMAIN"
fi
case "$PORT" in
    ''|*[!0-9]*) die "not a port: $PORT" ;;
esac
[ "$PORT" -ge 1 ] && [ "$PORT" -le 65535 ] || die "not a port: $PORT"

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
    PSK=hex:$(od -An -tx1 -N32 /dev/urandom | tr -d ' \n')
    [ ${#PSK} -eq 68 ] || die "cannot make a pre-shared key"
    {
        echo "# camex server settings, made by install.sh; see"
        echo "# https://github.com/OpenIPC/camex/blob/master/examples/camex.conf"
        echo "[server]"
        echo "port=$PORT"
        echo "# The server's address in the tunnel and the clients' pool"
        echo "local_cidr=10.200.0.1/24"
        echo "auto_pool=10.200.0.0/24"
        echo "# Clients do not reach each other"
        echo "client_isolation=yes"
        echo "# The clients' pre-shared key"
        echo "psk=$PSK"
        [ -n "$DOMAIN" ] && echo "domain=$DOMAIN"
    } > "$TMP/camex.conf"
    (umask 077; cp "$TMP/camex.conf" "$CONF") || die "cannot write $CONF"
    chmod 600 "$CONF"
    say "created: $CONF (port $PORT, a new psk)"
fi

# 6. The service ------------------------------------------------------------
step 6 "The camex service"
if [ ! -f "$DEFAULTS" ]; then
    # The arguments of a unit made by hand (camex 3.9 and before) stay
    args=$(sed -n 's/^Environment="\{0,1\}CAMEX_ARGS=\(.*\)/\1/p' \
           "$UNITS/camex.service" 2>/dev/null | sed 's/"$//' | tail -n 1)
    case "$args" in
        ''|*SERVER_PUBLIC_KEY*)
            args="--mode server --config $CONF --server-key-file $KEY" ;;
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
        put "$TMP/nginx.conf" "$NGINX_CONF" 644
    fi
    if [ ! -f "$MAP" ]; then
        printf 'map $host $camex_upstream {\n}\n' > "$MAP"
        say "created: $MAP (empty)"
    fi
    # The start page's password: user camex, made once
    pw_new=
    if [ ! -s "$PASSWORD_FILE" ]; then
        (umask 077; openssl rand -hex 10 > "$PASSWORD_FILE") ||
            die "cannot make the password"
        pw_new=1
        say "created: $PASSWORD_FILE"
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
    # The sync script and its timer
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
    fetch "$CAMEX_SRC/examples/camex-nginx-sync.timer" "$TMP/sync.timer"
    put "$TMP/sync.service" "$UNITS/camex-nginx-sync.service" 644 || true
    put "$TMP/sync.timer" "$UNITS/camex-nginx-sync.timer" 644 || true
    systemctl daemon-reload
    nginx -t 2>"$TMP/nginx-t" || { cat "$TMP/nginx-t" >&2; die "nginx -t rejects the configuration"; }
    if systemctl is-active --quiet nginx; then
        systemctl reload nginx || die "systemctl reload nginx failed"
    else
        systemctl enable --now nginx >/dev/null 2>&1 || die "nginx did not start"
    fi
    systemctl enable --now camex-nginx-sync.timer >/dev/null 2>&1 ||
        die "cannot enable camex-nginx-sync.timer"
    systemctl start camex-nginx-sync.service ||
        die "the first sync failed: journalctl -u camex-nginx-sync"
    say "checking: $SYNC --check"
    "$SYNC" --check || say "see the FAIL lines above (DNS is yours to set up)"
fi

# 8. The result -------------------------------------------------------------
step 8 "Done"
# What camex really uses: its command line wins over camex.conf (an older
# unit's arguments moved to /etc/default/camex may have --psk or --port)
ARGS=$(sed -n 's/^CAMEX_ARGS="\{0,1\}\([^"]*\)"\{0,1\}$/\1/p' "$DEFAULTS" | tail -n 1)
USED_CONF=; USED_KEY=; ARG_PSK=; ARG_PORT=
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
    esac
    shift
done
if [ "$USED_CONF" != "$CONF" ]; then
    say "WARNING: camex.service reads ${USED_CONF:-no config file}, not $CONF" \
        "(CAMEX_ARGS in $DEFAULTS): the settings made here are not used"
fi
[ -n "$USED_KEY" ] && [ "$USED_KEY" != "$KEY" ] && [ -r "$USED_KEY" ] &&
    PUBKEY=$(sed -n 's/^public_key=//p' "$USED_KEY")
PSK=${ARG_PSK:-$(conf_server psk "${USED_CONF:-$CONF}")}
PORT_NOW=${ARG_PORT:-$(conf_server port "${USED_CONF:-$CONF}")}
echo
echo "camex server is running: systemctl status camex, camex --status"
echo "Open UDP port ${PORT_NOW:-5800} in the firewall. A client connects with:"
echo
echo "  camex --mode client --auto --server-host <this host> --port ${PORT_NOW:-5800} \\"
echo "      --server-pubkey $PUBKEY --psk '${PSK:-<psk= of $CONF>}'"
if [ -n "$DOMAIN" ]; then
    echo
    echo "Clients' web UIs: http://<client ID>.$DOMAIN/"
    echo "Start page:       http://$DOMAIN/  (user camex, password $(cat "$PASSWORD_FILE"))"
    echo "DNS records *.$DOMAIN and $DOMAIN must point to this host (docs/nginx.md)."
fi
echo
echo "Files and how to remove them: install.sh --uninstall"

exit 0
}
