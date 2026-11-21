#!/bin/sh
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# entrypoint.sh — start the camex server; on the first start, make its key
# pair and settings in /etc/camex (the compose file's ./data), as install.sh
# does. Arguments are passed on to camex.
#
#   CAMEX_PORT     UDP and TCP port (5800); first start only
#   CAMEX_POOL     the clients' pool, the server at its first address
#                  (10.200.0.0/24); first start only
#   CAMEX_NETWORK  the network clients join by name (20 random letters and
#                  digits); first start only, at least 16 characters
#
set -eu

ETC=/etc/camex
KEY=$ETC/server.key
CONF=$ETC/camex.conf
PORT=${CAMEX_PORT:-5800}
POOL=${CAMEX_POOL:-10.200.0.0/24}

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
    # The server at the pool's first address
    FIRST=$(echo "$POOL" | awk -F'[./]' '{ printf "%d.%d.%d.%d/%d", $1, $2, $3, $4 + 1, $5 }')
    {
        echo "# camex server settings, made by the container's first start; see"
        echo "# https://github.com/OpenIPC/camex/blob/master/examples/camex.conf"
        if [ -n "$NET" ]; then
            echo "# The network clients join with: camex --server-host <host> --network <name>"
            printf '[network %s]\n\n' "$NET"
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
    } > "$CONF.new"
    mv -f "$CONF.new" "$CONF"
    echo "camex: created $CONF"
    if [ -n "$NET" ]; then
        echo "camex: clients join with: camex --server-host <this host> --port $PORT --network $NET" \
            "--server-pubkey $(sed -n 's/^public_key=//p' "$KEY")"
        echo "camex: the key is public (fine in firmware); --network alone works too, but then" \
            "networks a client makes show as net-... instead of their names"
    else
        echo "camex: clients join with: camex --mode client --auto --server-host <this host> --port $PORT" \
            "--server-pubkey $(sed -n 's/^public_key=//p' "$KEY") --psk '$PSK'"
    fi
fi

exec camex --mode server --config "$CONF" --server-key-file "$KEY" "$@"
