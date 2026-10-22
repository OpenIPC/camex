#!/bin/sh
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# camex-nginx-sync.sh — keep nginx's map of camex clients up to date (#84)
#
# Writes `camex --status --format nginx-map` to $MAP when it changed, then
# checks and reloads nginx; on a failed check the previous map is put back.
# A server that does not answer leaves the map as it is.
#
# Usage: camex-nginx-sync.sh            once (cron, a systemd timer)
#        camex-nginx-sync.sh --loop 10  every 10 seconds
#
# Settings (environment):
#   DOMAIN         the domain after the client ID (cam.example.lan)
#   UPSTREAM_PORT  the clients' web port (80)
#   DEFAULT        where unknown names go, host:port (empty: nginx's
#                  offline page in nginx-camex.conf)
#   CONTROL        one server's control socket (empty: every server)
#   MAP            the map file nginx includes (/etc/nginx/camex-map.conf)
#   CAMEX, NGINX_BIN  the programs (camex, nginx; not NGINX, which nginx
#                  itself reads)
#

set -u

DOMAIN=${DOMAIN-cam.example.lan}
UPSTREAM_PORT=${UPSTREAM_PORT:-80}
DEFAULT=${DEFAULT-}
CONTROL=${CONTROL-}
MAP=${MAP:-/etc/nginx/camex-map.conf}
CAMEX=${CAMEX:-camex}
NGINX_BIN=${NGINX_BIN:-nginx}

log() {
    echo "camex-nginx-sync: $*" >&2
}

sync_once() {
    tmp="$MAP.tmp.$$"
    set -- --status --format nginx-map --upstream-port "$UPSTREAM_PORT"
    [ -n "$DOMAIN" ] && set -- "$@" --domain "$DOMAIN"
    [ -n "$DEFAULT" ] && set -- "$@" --default "$DEFAULT"
    [ -n "$CONTROL" ] && set -- "$@" --control "$CONTROL"
    if ! "$CAMEX" "$@" > "$tmp"; then
        rm -f "$tmp"
        log "no map (a server did not answer); $MAP left as it is"
        return 1
    fi
    if [ -f "$MAP" ] && cmp -s "$tmp" "$MAP"; then
        rm -f "$tmp"
        return 0
    fi
    [ -f "$MAP" ] && cp -p "$MAP" "$MAP.prev"
    mv "$tmp" "$MAP"
    if "$NGINX_BIN" -t >/dev/null 2>&1 && "$NGINX_BIN" -s reload; then
        log "map updated: $(grep -c ':[0-9]*;$' "$MAP") clients, nginx reloaded"
        return 0
    fi
    log "nginx rejected the new map; the previous one is back"
    if [ -f "$MAP.prev" ]; then
        mv "$MAP.prev" "$MAP"
    else
        rm -f "$MAP"
    fi
    return 1
}

if [ "${1-}" = "--loop" ]; then
    interval=${2:-10}
    while :; do
        sync_once
        sleep "$interval"
    done
fi
sync_once
