#!/bin/sh
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# entrypoint.sh — start the camex server; on the first start, make its key
# pair and settings in /etc/camex (the compose file's ./data), as install.sh
# does. Arguments are passed on to camex.
#
#   CAMEX_PORT     UDP and TCP port (5800); first start only
#   CAMEX_POOL     the pool of the clients that join with --psk, the server
#                  at its first address (10.200.0.0/24); any address in it
#                  gives its network; first start only. The network's
#                  clients get a pool of their own (docs/networks.md)
#   CAMEX_NETWORK  the network clients join by name (20 random letters and
#                  digits); first start only, at least 16 characters
#   CAMEX_DOMAIN   the cameras' domain for nginx on the host: domain= and
#                  web_listen=yes, camex's start page and nginx's lookup on
#                  127.0.0.1:5801 (docs/nginx.md); first start only
#
set -eu

ETC=/etc/camex
KEY=$ETC/server.key
CONF=$ETC/camex.conf
PORT=${CAMEX_PORT:-5800}
POOL=${CAMEX_POOL:-10.200.0.0/24}

# The pool as install.sh --pool takes it (#205): any address in it gives its
# network (10.60.1.7/22 -> 10.60.0.0/22), the server at the first address;
# "<network>/<prefix> <first>/<prefix>", empty when it is not a pool
pool_of() {
    echo "$1" | awk -F/ 'NF == 2 && $2 ~ /^[0-9]+$/ && $2 >= 8 && $2 <= 30 {
        n = split($1, o, ".")
        if (n != 4) exit
        a = 0
        for (i = 1; i <= 4; i++) {
            if (o[i] !~ /^[0-9]+$/ || o[i] > 255) exit
            a = a * 256 + o[i]
        }
        size = 2 ^ (32 - $2)
        net = a - a % size
        printf "%d.%d.%d.%d/%d %d.%d.%d.%d/%d\n",
            int(net / 16777216), int(net / 65536) % 256, int(net / 256) % 256, net % 256, $2,
            int((net + 1) / 16777216), int((net + 1) / 65536) % 256, int((net + 1) / 256) % 256,
            (net + 1) % 256, $2
    }'
}

umask 077
mkdir -p "$ETC" /run/camex
chmod 755 /run/camex

if [ ! -s "$KEY" ]; then
    camex --genkey > "$KEY.new" && mv -f "$KEY.new" "$KEY"
    echo "camex: created $KEY"
fi

# Named networks came with camex 5.0.0: an older release gets psk= only
NETWORKS=
camex --help 2>&1 | grep -q -- '--network' && NETWORKS=1

if [ ! -f "$CONF" ]; then
    NET=
    [ -n "$NETWORKS" ] &&
        NET=${CAMEX_NETWORK:-$(LC_ALL=C tr -dc 'a-z0-9' </dev/urandom | head -c 20)}
    PSK=hex:$(od -An -tx1 -N32 /dev/urandom | tr -d ' \n')
    P=$(pool_of "$POOL")
    [ -n "$P" ] || { echo "camex: CAMEX_POOL=$POOL is not <address>/<8..30>" >&2; exit 1; }
    POOL=${P% *}
    FIRST=${P#* }
    case "${CAMEX_DOMAIN:-}" in
        ""|*[!A-Za-z0-9.-]*|.*|*.|*..*)
            [ -z "${CAMEX_DOMAIN:-}" ] ||
                { echo "camex: CAMEX_DOMAIN=$CAMEX_DOMAIN is not a domain" >&2; exit 1; } ;;
    esac
    {
        echo "# camex server settings, made by the container's first start; see"
        echo "# https://github.com/OpenIPC/camex/blob/master/examples/camex.conf"
        if [ -n "$NET" ]; then
            echo "# The network clients join with: camex --server-host <host> --network <name>"
            printf '[network %s]\n' "$NET"
            echo "# Shown in the log and --status instead of the name, which is the key"
            printf 'label = main\n\n'
        fi
        cat <<CONF
[server]
port=$PORT
# The server's address in the tunnel and the clients' pool
local_cidr=$FIRST
auto_pool=$POOL
# Clients do not reach each other
client_isolation=yes
# Seconds of silence before a client is dropped
client_timeout=60
# The pre-shared key of clients that join with --psk
psk=$PSK
CONF
        if [ -n "${CAMEX_DOMAIN:-}" ]; then
            echo "# The cameras' domain; the start page and nginx's lookup on 127.0.0.1:5801"
            echo "domain=$CAMEX_DOMAIN"
            echo "web_listen=yes"
        fi
    } > "$CONF.new"
    mv -f "$CONF.new" "$CONF"
    echo "camex: created $CONF"
    # The network's name is its key, and the psk is one too: they stay in
    # camex.conf, not in the container's log, which Docker keeps
    PUB=$(sed -n 's/^public_key=//p' "$KEY")
    if [ -n "$NET" ]; then
        echo "camex: clients join with: camex --server-host <this host> --port $PORT" \
            "--network <name> --server-pubkey $PUB"
        echo "camex: <name> is that of the [network NAME] block in data/camex.conf (keep it" \
            "secret: it is the network's key); the server's key is public (fine in firmware)"
    else
        echo "camex: clients join with: camex --mode client --auto --server-host <this host>" \
            "--port $PORT --server-pubkey $PUB --psk <psk= of data/camex.conf>"
    fi
fi

exec camex --mode server --config "$CONF" --server-key-file "$KEY" "$@"
