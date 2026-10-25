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
#        camex-nginx-sync.sh --check    check the whole setup: the servers,
#                                       the domain, nginx, the start page,
#                                       DNS; exit code 1 when something is
#                                       wrong (#106)
#
# Settings (environment):
#   DOMAIN         the domain after the client ID; empty (the default):
#                  domain= in [server] of the server's file (#106)
#   UPSTREAM_PORT  the clients' web port (80)
#   DEFAULT        where unknown names go, <IPv4>:<port> (empty: nginx's
#                  offline page in nginx-camex.conf)
#   CONTROL        one server's control socket (empty: every server)
#   MAP            the map file nginx includes (/etc/nginx/camex-map.conf)
#   INDEX          the start page nginx serves on the bare domain, e.g.
#                  /var/lib/camex-nginx/index.html (empty: no updates; a
#                  page written before stays until it is removed); its
#                  directory must exist
#   CAMEX, NGINX_BIN  the programs (camex, nginx; not NGINX, which nginx
#                  itself reads)
#

set -u

if ! command -v flock >/dev/null 2>&1; then
    echo "camex-nginx-sync: needs flock (util-linux or busybox)" >&2
    exit 2
fi

DOMAIN=${DOMAIN-}
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
    # The page links to names in the map. It is built first, then the map:
    # every device on it was connected before the map was asked for, so
    # the map has it. It goes out only once that map is applied; while
    # nginx rejects a new map or does not reload, the old page stays
    index_build
    irc=$?
    sync_locked
    rc=$?
    replaced=
    if [ $rc -ne 0 ]; then
        [ $irc -eq 0 ] && log "the map was not applied: $INDEX left as it is"
        rm -f "${itmp-}"
    elif [ $irc -eq 0 ]; then
        index_publish || rc=1
    elif [ $irc -eq 1 ]; then
        rc=1
    fi
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
        nginx_warnings
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

# The start page into $itmp: 0, 1 (a server did not answer, logged) or 2
# (no INDEX, no page)
index_build() {
    [ -n "$INDEX" ] || return 2
    itmp="$INDEX.tmp.$$"
    # (checked with true first: a failed redirection must not be taken
    # for a server that did not answer)
    if ! true 2>/dev/null > "$itmp"; then
        log "cannot write $itmp (does the directory of INDEX exist?);" \
            "$INDEX left as it is"
        return 1
    fi
    set -- --status --format nginx-index
    [ -n "$DOMAIN" ] && set -- "$@" --domain "$DOMAIN"
    [ -n "$CONTROL" ] && set -- "$@" --control "$CONTROL"
    if ! "$CAMEX" "$@" > "$itmp" 9>&-; then
        rm -f "$itmp"
        # camex said why above: no server answered, an older server, ...
        log "no start page (see the error above); $INDEX left as it is"
        return 1
    fi
    return 0
}

# $itmp in place of $INDEX when it changed: one rename, so nginx never
# serves half of it. The page shows ages and traffic, so it changes with
# almost every run: the log says so only when the devices online did
index_publish() {
    if [ -f "$INDEX" ] && cmp -s "$itmp" "$INDEX"; then
        rm -f "$itmp"
        # Readable for nginx even when someone wrote it with a tight umask
        chmod 644 "$INDEX" 2>/dev/null ||
            log "cannot make $INDEX readable for nginx"
        return 0
    fi
    before=$(grep -c '^<tr class="dev">' "$INDEX" 2>/dev/null)
    # nginx's workers read it: readable by all, whatever the umask
    if ! { chmod 644 "$itmp" && mv -f "$itmp" "$INDEX"; }; then
        rm -f "$itmp"
        log "cannot write $INDEX; left as it is"
        return 1
    fi
    devices=$(grep -c '^<tr class="dev">' "$INDEX")
    if [ "$devices" != "$before" ]; then
        [ "$devices" = 1 ] && noun=device || noun=devices
        log "start page updated: $devices $noun online"
    fi
    return 0
}

# The domain the map was built for: its "# domain:" line (camex 3.8.0)
map_domain() {
    sed -n 's/^# domain: //p' "$1" 2>/dev/null | head -n 1
}

# A domain as an extended regular expression
domain_re() {
    printf '%s' "$1" | sed 's/[.]/\\./g'
}

# After a new map was applied: what keeps nginx from following it (#106)
nginx_warnings() {
    dom=$(map_domain "$MAP")
    case "$dom" in
        ''|'(none)')
            log "warning: no domain (domain= in [server] of camex.conf," \
                "or DOMAIN=): the names in the map are bare client IDs"
            return 0 ;;
    esac
    conf=$("$NGINX_BIN" -T 2>/dev/null 9>&-) || return 0
    re=$(domain_re "$dom")
    printf '%s\n' "$conf" | grep -qE "^[[:space:]]*include[[:space:]]+$MAP;" ||
        log "warning: nginx's configuration does not include $MAP"
    printf '%s\n' "$conf" | grep -E '^[[:space:]]*server_name[[:space:]]' |
        grep -qiE "[[:space:]](\*)?\.$re([[:space:];]|$)" ||
        log "warning: nginx has no server_name *.$dom: the cameras' names" \
            "do not reach the map (nginx-camex.conf)"
}

# --check: one line per item, ok / WARN / FAIL; exit code 1 on a FAIL
check_all() {
    fails=0
    ok()   { echo "  ok    $*"; }
    warn() { echo "  WARN  $*"; }
    fail() { echo "  FAIL  $*"; fails=$((fails + 1)); }

    echo "camex-nginx-sync --check"
    if ! command -v "$CAMEX" >/dev/null 2>&1; then
        fail "$CAMEX not found (CAMEX=)"
        return 1
    fi
    set -- --status --format nginx-map
    [ -n "$DOMAIN" ] && set -- "$@" --domain "$DOMAIN"
    [ -n "$CONTROL" ] && set -- "$@" --control "$CONTROL"
    ctmp=$(mktemp) || return 1
    if "$CAMEX" "$@" > "$ctmp" 2> "$ctmp.err"; then
        ok "the servers answer: $(grep -c '^    "' "$ctmp") clients in the map"
    else
        fail "the servers do not answer: $(tail -n 1 "$ctmp.err")"
    fi
    dom=$(map_domain "$ctmp")
    rm -f "$ctmp" "$ctmp.err"
    case "$dom" in
        ''|'(none)')
            fail "no domain: set domain= in [server] of camex.conf (or DOMAIN=)" ;;
        openipc.click)
            warn "the domain is the examples' openipc.click: right only if" \
                 "it is yours (domain= in [server])" ;;
        *)
            ok "domain: $dom" ;;
    esac

    if ! command -v "$NGINX_BIN" >/dev/null 2>&1; then
        fail "$NGINX_BIN not found (NGINX_BIN=)"
    elif ! conf=$("$NGINX_BIN" -T 2>/dev/null); then
        fail "nginx -T fails: check the configuration with nginx -t"
    else
        ok "nginx accepts its configuration"
        if printf '%s\n' "$conf" |
           grep -qE "^[[:space:]]*include[[:space:]]+$MAP;"; then
            ok "nginx includes the map $MAP"
        else
            fail "nginx does not include the map $MAP (nginx-camex.conf)"
        fi
        case "$dom" in
            ''|'(none)') ;;
            *)
                re=$(domain_re "$dom")
                names=$(printf '%s\n' "$conf" |
                        grep -E '^[[:space:]]*server_name[[:space:]]')
                if printf '%s\n' "$names" |
                   grep -qiE "[[:space:]](\*)?\.$re([[:space:];]|$)"; then
                    ok "nginx has server_name *.$dom"
                else
                    fail "nginx has no server_name *.$dom (nginx-camex.conf)"
                fi
                if [ -n "$INDEX" ]; then
                    if printf '%s\n' "$names" |
                       grep -qiE "[[:space:]]$re([[:space:];]|$)"; then
                        ok "nginx has server_name $dom (the start page)"
                    else
                        warn "nginx has no server_name $dom: no start page" \
                             "on the bare domain"
                    fi
                fi ;;
        esac
    fi

    if [ -n "$INDEX" ]; then
        if [ ! -d "$(dirname "$INDEX")" ]; then
            fail "the start page's directory $(dirname "$INDEX") is missing"
        elif [ ! -f "$INDEX" ]; then
            warn "no start page $INDEX yet: run the sync"
        elif [ -n "$(find "$INDEX" -perm -004 2>/dev/null)" ]; then
            ok "start page $INDEX"
        else
            fail "start page $INDEX is not readable for nginx"
        fi
    else
        ok "no start page (INDEX= empty)"
    fi

    case "$dom" in
        ''|'(none)') ;;
        *)
            # DNS: a name nobody uses must resolve to this host (wildcard)
            mine=$( (hostname -I 2>/dev/null; ip -o addr show 2>/dev/null |
                     awk '{sub("/.*", "", $4); print $4}') | tr ' ' '\n')
            for h in "camex-check-$$.$dom" ${INDEX:+"$dom"}; do
                a=$(getent hosts "$h" 2>/dev/null | awk '{print $1; exit}')
                if [ -z "$a" ]; then
                    warn "$h does not resolve: a DNS record for" \
                         "$( [ "$h" = "$dom" ] && echo "$dom" || echo "*.$dom")?"
                elif printf '%s\n' "$mine" | grep -qx "$a"; then
                    ok "$h resolves to this host ($a)"
                else
                    warn "$h resolves to $a, not an address of this host"
                fi
            done ;;
    esac

    if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
        if systemctl is-active --quiet camex-nginx-sync.timer; then
            ok "camex-nginx-sync.timer is active"
        else
            warn "camex-nginx-sync.timer is not active (systemctl enable" \
                 "--now camex-nginx-sync.timer)"
        fi
    fi
    [ "$fails" -eq 0 ] && echo "all good" || echo "$fails problem(s)"
    [ "$fails" -eq 0 ]
}

if [ "${1-}" = "--check" ]; then
    check_all
    exit $?
fi

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
