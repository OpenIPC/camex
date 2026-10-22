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
#   DEFAULT        where unknown names go, <IPv4>:<port> (empty: nginx's
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

# One sync at a time: a second one waits, so a rollback never undoes
# another run's map. mkdir is atomic; a lock older than 5 minutes is
# taken as left behind by a run that was killed.
LOCK="$MAP.lock"
lock() {
    tries=0
    until mkdir "$LOCK" 2>/dev/null; do
        if [ -n "$(find "$LOCK" -maxdepth 0 -mmin +5 2>/dev/null)" ]; then
            rmdir "$LOCK" 2>/dev/null
            continue
        fi
        tries=$((tries + 1))
        if [ "$tries" -ge 60 ]; then
            log "another sync holds $LOCK; giving up"
            return 1
        fi
        sleep 1
    done
}
unlock() {
    rmdir "$LOCK" 2>/dev/null
}

# The map nginx last accepted and reloaded: a map counts as applied only
# then, so a failed reload (nginx not running yet, say) is tried again by
# the next run even though the file did not change
APPLIED="$MAP.applied"

# Interrupted between replacing the map and a reload: put back what was
# there, drop the temporary files and the lock
cleanup() {
    if [ -n "${replaced-}" ]; then
        if [ -f "${prev-}" ]; then
            mv "$prev" "$MAP"
        else
            rm -f "$MAP"
        fi
    fi
    rm -f "${tmp-}" "${prev-}"
    unlock
}

sync_once() {
    lock || return 1
    trap 'cleanup; exit 1' INT TERM HUP
    replaced=
    sync_locked
    rc=$?
    replaced=
    trap - INT TERM HUP
    unlock
    return $rc
}

sync_locked() {
    tmp="$MAP.tmp.$$"
    prev="$MAP.prev.$$"
    set -- --status --format nginx-map --upstream-port "$UPSTREAM_PORT"
    [ -n "$DOMAIN" ] && set -- "$@" --domain "$DOMAIN"
    [ -n "$DEFAULT" ] && set -- "$@" --default "$DEFAULT"
    [ -n "$CONTROL" ] && set -- "$@" --control "$CONTROL"
    if ! "$CAMEX" "$@" > "$tmp"; then
        rm -f "$tmp"
        log "no map (a server did not answer); $MAP left as it is"
        return 1
    fi
    if [ -f "$MAP" ] && cmp -s "$tmp" "$MAP" && [ -f "$APPLIED" ] &&
       cmp -s "$tmp" "$APPLIED"; then
        rm -f "$tmp"
        return 0
    fi
    # This run's own backup: the rollback puts back exactly what it replaced
    if [ -f "$MAP" ] && ! cp -p "$MAP" "$prev"; then
        rm -f "$tmp" "$prev"
        log "cannot back up $MAP; left as it is"
        return 1
    fi
    if ! mv "$tmp" "$MAP"; then
        rm -f "$tmp" "$prev"
        log "cannot write $MAP; left as it is"
        return 1
    fi
    replaced=1
    if ! "$NGINX_BIN" -t >/dev/null 2>&1; then
        log "nginx rejected the new map; the previous one is back"
    elif ! "$NGINX_BIN" -s reload; then
        log "nginx did not reload (is it running? start it, e.g. systemctl" \
            "start nginx); the map is kept and the next run reloads again"
        replaced=
        rm -f "$prev" "$APPLIED"
        return 1
    else
        replaced=
        rm -f "$prev"
        cp -p "$MAP" "$APPLIED" 2>/dev/null
        clients=$(grep -c '^    "' "$MAP")
        log "map updated: $clients clients, nginx reloaded"
        return 0
    fi
    replaced=
    if [ -f "$prev" ]; then
        mv "$prev" "$MAP" || log "cannot put back $MAP from $prev"
    else
        rm -f "$MAP"
    fi
    return 1
}

if [ "${1-}" = "--loop" ]; then
    interval=${2-10}
    case "$interval" in
        ''|*[!0-9]*|0) log "--loop needs a whole number of seconds above 0"
                       exit 2 ;;
    esac
    while :; do
        sync_once
        sleep "$interval" || exit 1
    done
fi
sync_once
