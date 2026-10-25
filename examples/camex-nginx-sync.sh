#!/bin/sh
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# camex-nginx-sync.sh — keep nginx's map of camex clients up to date (#84)
#
# Writes `camex --status --format nginx-map` to $MAP when it changed, then
# checks and reloads nginx; on a failed check the previous map is put back.
# With INDEX set, also writes the start page of the clients (`camex
# --status --format nginx-index`, #105) there when it changed; a static
# file, so nginx needs no reload for it. A server that does not answer
# leaves the map and the page as they are.
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
#   INDEX          the start page nginx serves on the bare domain, e.g.
#                  /var/lib/camex-nginx/index.html (empty: no page); its
#                  directory must exist
#   CAMEX, NGINX_BIN  the programs (camex, nginx; not NGINX, which nginx
#                  itself reads)
#

set -u

if ! command -v flock >/dev/null 2>&1; then
    echo "camex-nginx-sync: needs flock (util-linux or busybox)" >&2
    exit 2
fi

DOMAIN=${DOMAIN-cam.example.lan}
UPSTREAM_PORT=${UPSTREAM_PORT:-80}
DEFAULT=${DEFAULT-}
CONTROL=${CONTROL-}
MAP=${MAP:-/etc/nginx/camex-map.conf}
INDEX=${INDEX-}
CAMEX=${CAMEX:-camex}
NGINX_BIN=${NGINX_BIN:-nginx}

log() {
    echo "camex-nginx-sync: $*" >&2
}

# One sync at a time: a second one waits, so a rollback never undoes
# another run's map. flock(1) (util-linux, busybox): the kernel drops the
# lock when its holder exits, so a killed run leaves nothing behind and a
# slow one keeps it for as long as it runs. The programs it starts do not
# inherit the lock's descriptor (9>&-).
LOCK="$MAP.lock"
lock() {
    # (checked with true first: a failed redirection of exec ends sh)
    if ! true 2>/dev/null >>"$LOCK"; then
        log "cannot open $LOCK"
        return 1
    fi
    exec 9>>"$LOCK"
    if ! flock -w 60 9; then
        exec 9>&-
        log "another sync holds $LOCK; giving up"
        return 1
    fi
}
unlock() {
    exec 9>&-
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
    rm -f "${tmp-}" "${prev-}" "${itmp-}"
    unlock
}

sync_once() {
    lock || return 1
    trap 'cleanup; exit 1' INT TERM HUP
    replaced=
    sync_locked
    rc=$?
    replaced=
    sync_index || rc=1
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
    if ! "$CAMEX" "$@" > "$tmp" 9>&-; then
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
    if ! check=$("$NGINX_BIN" -t 2>&1 9>&-); then
        log "nginx rejected the new map; the previous one is back:"
        printf '%s\n' "$check" >&2
    elif ! { replaced=; "$NGINX_BIN" -s reload 9>&-; }; then
        # (replaced cleared first: once nginx was told to reload, an
        # interrupted run keeps the new map and the next run settles it)
        log "nginx did not reload (is it running? start it, e.g. systemctl" \
            "start nginx); the map is kept and the next run reloads again"
        replaced=
        rm -f "$prev" "$APPLIED"
        return 1
    else
        rm -f "$prev"
        # Through a temporary file: the marker is the whole map or absent
        if ! { cp -p "$MAP" "$APPLIED.tmp.$$" && mv "$APPLIED.tmp.$$" "$APPLIED"; }; then
            rm -f "$APPLIED.tmp.$$" "$APPLIED"
            log "nginx reloaded, but $APPLIED cannot be written: the next run reloads again"
            return 1
        fi
        clients=$(grep -c '^    "' "$MAP")
        [ "$clients" = 1 ] && noun=client || noun=clients
        log "map updated: $clients $noun, nginx reloaded"
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

# The start page: replaced in one step (rename) when it changed, so nginx
# never serves half of it
sync_index() {
    [ -n "$INDEX" ] || return 0
    itmp="$INDEX.tmp.$$"
    set -- --status --format nginx-index
    [ -n "$DOMAIN" ] && set -- "$@" --domain "$DOMAIN"
    [ -n "$CONTROL" ] && set -- "$@" --control "$CONTROL"
    if ! "$CAMEX" "$@" > "$itmp" 9>&-; then
        rm -f "$itmp"
        log "no start page (a server did not answer); $INDEX left as it is"
        return 1
    fi
    if [ -f "$INDEX" ] && cmp -s "$itmp" "$INDEX"; then
        rm -f "$itmp"
        return 0
    fi
    # nginx's workers read it: readable by all, whatever the umask
    if ! { chmod 644 "$itmp" && mv -f "$itmp" "$INDEX"; }; then
        rm -f "$itmp"
        log "cannot write $INDEX; left as it is"
        return 1
    fi
    devices=$(grep -c '^<tr class="dev">' "$INDEX")
    [ "$devices" = 1 ] && noun=device || noun=devices
    log "start page updated: $devices $noun"
    return 0
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
