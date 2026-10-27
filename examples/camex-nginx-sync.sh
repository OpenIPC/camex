#!/bin/sh
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# Installed as: /usr/local/bin/camex-nginx-sync
# Docs: https://github.com/OpenIPC/camex/blob/master/docs/nginx.md#sync-script
#

set -u

if ! command -v flock >/dev/null 2>&1; then
    echo "camex-nginx-sync: needs flock (util-linux or busybox)" >&2
    exit 2
fi

DOMAIN_SET=${DOMAIN+set}  # DOMAIN= given, even empty: --check keeps it
DOMAIN=${DOMAIN-}
# Which settings were given (even empty): --check keeps those, and takes
# the others from the installed service
UPSTREAM_PORT_SET=${UPSTREAM_PORT+set}
DEFAULT_SET=${DEFAULT+set}
CONTROL_SET=${CONTROL+set}
MAP_SET=${MAP+set}
HTTP_PORT_SET=${HTTP_PORT+set}
UPSTREAM_PORT=${UPSTREAM_PORT:-80}
# nginx's port: in the start page's links and what --check asks (#141)
HTTP_PORT=${HTTP_PORT:-80}
DEFAULT=${DEFAULT-}
CONTROL=${CONTROL-}
MAP=${MAP:-/etc/nginx/camex-map.conf}
# The start page's password, as install.sh keeps it (#119)
PASSWORD_FILE=${PASSWORD_FILE:-/etc/camex/nginx-password}
INDEX_SET=${INDEX+set}    # INDEX= given, even empty: --check keeps it
INDEX=${INDEX-}
CAMEX=${CAMEX:-camex}
NGINX_BIN=${NGINX_BIN:-nginx}

log() {
    echo "camex-nginx-sync: $*" >&2
}

# Run a command; its messages (camex's warnings: no domain, bad IDs, ...)
# go to the log only when they differ from the previous run's, kept in
# $1, so a lasting state is not logged every 10 seconds
quiet_repeats() {
    seen=$1
    shift
    werr=$(mktemp "$seen.XXXXXX" 2>/dev/null) || { "$@"; return $?; }
    "$@" 2> "$werr"
    wrc=$?
    # A failure always says why; warnings of a good run once
    if [ "$wrc" -eq 0 ] && [ -f "$seen" ] && cmp -s "$werr" "$seen"; then
        rm -f "$werr"
    else
        cat "$werr" >&2
        mv -f "$werr" "$seen" 2>/dev/null || rm -f "$werr"
    fi
    return $wrc
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
    if ! quiet_repeats "$MAP.warnings" "$CAMEX" "$@" > "$tmp" 9>&-; then
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
    # (camex before 3.12.0 has no --http-port: only when it is needed)
    [ "$HTTP_PORT" != 80 ] && set -- "$@" --http-port "$HTTP_PORT"
    [ -n "$CONTROL" ] && set -- "$@" --control "$CONTROL"
    if ! quiet_repeats "$MAP.warnings-index" "$CAMEX" "$@" > "$itmp" 9>&-; then
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

# nginx -T output (in $1) includes $MAP: compared as text, not a pattern
includes_map() {
    printf '%s\n' "$1" |
        sed -e 's/[[:space:]][[:space:]]*/ /g' -e 's/^ //' -e 's/ $//' |
        grep -qxF "include $MAP;"
}

# nginx's workers (not root) can reach file $1: every directory above it
# lets others through
reachable() {
    p=$1
    while [ "$p" != / ] && [ "$p" != . ]; do
        p=$(dirname "$p")
        [ -n "$(find "$p" -maxdepth 0 -perm -001 2>/dev/null)" ] || return 1
    done
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
    includes_map "$conf" ||
        log "warning: nginx's configuration does not include $MAP"
    printf '%s\n' "$conf" | grep -E '^[[:space:]]*server_name[[:space:]]' |
        grep -qiE "[[:space:]](\*)?\.$re([[:space:];]|$)" ||
        log "warning: nginx has no server_name *.$dom: the cameras' names" \
            "do not reach the map (nginx-camex.conf)"
}

# Does a server block of nginx's configuration $1 (nginx -T) for the bare
# domain $2 ask for a password: auth_basic and its user file, and no
# auth_basic off in it (#119)
page_protected() {
    printf '%s\n' "$1" | awk -v dom="$2" '
        function code(s,   i, ch, q, r) {
            q = ""; r = ""
            for (i = 1; i <= length(s); i++) {
                ch = substr(s, i, 1)
                if (q == "" && ch == "#") break
                if (q == "" && (ch == "\"" || ch == "\047")) q = ch
                else if (ch == q) q = ""
                r = r ch
            }
            return r
        }
        {
            l = code($0)
            if (!inb && depth == 0 && l ~ /^[[:space:]]*server[[:space:]]*\{/) {
                inb = 1; text = ""; page = 0; ab = 0; uf = 0; off = 0
            }
            o = gsub(/\{/, "{", l); c = gsub(/\}/, "}", l)
            if (inb) {
                text = text " " l
                # The server level covers its locations; any "off" in the
                # block may let the page out
                if (depth == 1 && l ~ /^[[:space:]]*auth_basic_user_file[[:space:]]/) uf = 1
                if (l ~ /^[[:space:]]*auth_basic[[:space:]]+off[[:space:]]*;/) off = 1
                else if (depth == 1 && l ~ /^[[:space:]]*auth_basic[[:space:]]/) ab = 1
            }
            depth += o - c
            if (inb && depth <= 0) {
                depth = 0; inb = 0
                if (match(text, /server_name[^;]*;/)) {
                    n = split(substr(text, RSTART + 11, RLENGTH - 12), w, /[[:space:]]+/)
                    for (t = 1; t <= n; t++) if (tolower(w[t]) == dom) page = 1
                }
                if (page) { seen = 1; if (!(ab && uf && !off)) bad = 1 }
            }
        }
        END { exit (seen && !bad) ? 0 : 1 }'
}

# What nginx on this host answers for host $1 on HTTP_PORT, its HTTP status
# on the last line; when that port only redirects to HTTPS (certbot's
# redirect, say), what port 443 answers. More arguments go to curl
# (-u user:password)
probe() {
    ph=$1
    shift
    pb=$(curl -s -m 5 -H "Host: $ph" -w '\n%{http_code}' "$@" \
             "http://127.0.0.1:$HTTP_PORT/" 2>/dev/null)
    case "${pb##*
}" in
        301|302|307|308)
            # The name as TLS's server name too (SNI), not only Host:
            curl -sk -m 5 --resolve "$ph:443:127.0.0.1" -w '\n%{http_code}' \
                "$@" "https://$ph/" 2>/dev/null ;;
        *)
            printf '%s\n' "$pb" ;;
    esac
}

# --check: one line per item, ok / WARN / FAIL; exit code 1 on a FAIL
check_all() {
    fails=0
    ok()   { echo "  ok    $*"; }
    warn() { echo "  WARN  $*"; }
    fail() { echo "  FAIL  $*"; fails=$((fails + 1)); }

    echo "camex-nginx-sync --check"
    u=/etc/systemd/system/camex-nginx-sync.service
    # Run by hand: the service's settings, for those not given here
    if [ -f "$u" ]; then
        for v in INDEX DOMAIN DEFAULT UPSTREAM_PORT HTTP_PORT CONTROL MAP; do
            eval "given=\${${v}_SET-}"
            [ -n "$given" ] && continue
            val=$(sed -n "s/^Environment=$v=//p" "$u" | tail -n 1)
            [ -n "$val" ] || continue
            eval "$v=\$val"
            ok "$v=$val (from $u)"
        done
    fi
    if ! command -v "$CAMEX" >/dev/null 2>&1; then
        fail "$CAMEX not found (CAMEX=)"
        return 1
    fi
    set -- --status --format nginx-map --upstream-port "$UPSTREAM_PORT"
    [ -n "$DEFAULT" ] && set -- "$@" --default "$DEFAULT"
    [ -n "$DOMAIN" ] && set -- "$@" --domain "$DOMAIN"
    [ -n "$CONTROL" ] && set -- "$@" --control "$CONTROL"
    # Its own directory: no name in /tmp that someone else could predict
    cdir=$(mktemp -d) || return 1
    if "$CAMEX" "$@" > "$cdir/map" 2> "$cdir/err"; then
        ok "the servers answer: $(grep -c '^    "' "$cdir/map") clients in the map"
        # camex's own warnings: servers with other domains, bad IDs, ...
        while IFS= read -r line; do
            warn "${line#camex: }"
        done < "$cdir/err"
    else
        fail "the servers do not answer: $(tail -n 1 "$cdir/err")"
    fi
    dom=$(map_domain "$cdir/map")
    # The map nginx uses against the servers' state now
    if [ -f "$MAP" ] && ! cmp -s "$cdir/map" "$MAP"; then
        warn "$MAP differs from the servers' state now: run the sync" \
             "(is camex-nginx-sync.service running?)"
    fi
    rm -rf "$cdir"
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
        if includes_map "$conf"; then
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
                    # Without curl the page is not asked below: at least
                    # its server block must have a password (#119)
                    if ! command -v curl >/dev/null 2>&1 &&
                       ! page_protected "$conf" "$dom"; then
                        warn "nginx's server block for $dom has no" \
                             "auth_basic: the start page has no password" \
                             "(see auth_basic in nginx-camex.conf)"
                    fi
                fi ;;
        esac
    fi

    # What nginx really does on HTTP_PORT of this host (the configuration
    # read above may name the domain in another block or port)
    case "$dom" in
        ''|'(none)') ;;
        *)
            if [ -n "$DEFAULT" ]; then
                ok "nginx not asked for a name with no camera (DEFAULT= sends" \
                   "those to $DEFAULT)"
            elif command -v curl >/dev/null 2>&1; then
                b=$(probe "camex-check-$$.$dom")
                case "$b" in
                    *"camera is offline"*)
                        ok "nginx sends camex-check-$$.$dom to camex" \
                           "(the offline page of a name with no camera)" ;;
                    *)
                        fail "nginx does not send camex-check-$$.$dom" \
                             "to camex (no offline page): check the server" \
                             "block for *.$dom" ;;
                esac
            else
                warn "no curl: nginx not asked on port $HTTP_PORT"
            fi
            if [ -n "$INDEX" ] && command -v curl >/dev/null 2>&1; then
                b=$(probe "$dom")
                case "$b" in
                    *"Camex Devices"*)
                        warn "the start page on $dom has no password: see" \
                             "auth_basic in nginx-camex.conf"
                        ok "nginx serves the start page on $dom" ;;
                    *"
401")
                        ok "the start page on $dom asks for a password"
                        # With the password the setup keeps (root only)
                        pw=$(head -n 1 "$PASSWORD_FILE" 2>/dev/null)
                        if [ -z "$pw" ]; then
                            ok "nginx answers on $dom (the page itself not" \
                               "checked: no $PASSWORD_FILE)"
                        else
                            # To curl in a file of its own, not in ps
                            kf=$(mktemp) &&
                                printf 'user = "camex:%s"\n' "$pw" > "$kf" &&
                                b=$(probe "$dom" -K "$kf") || b=
                            [ -n "${kf-}" ] && rm -f "$kf"
                            case "$b" in
                                *"Camex Devices"*)
                                    ok "nginx serves the start page on $dom" ;;
                                *"
401")
                                    warn "the password in $PASSWORD_FILE does" \
                                         "not open the start page on $dom" \
                                         "(a new one: remove it, run" \
                                         "install.sh again)" ;;
                                *)
                                    warn "nginx does not serve the start page" \
                                         "on $dom" ;;
                            esac
                        fi ;;
                    *) warn "nginx does not serve the start page on $dom" ;;
                esac
            fi ;;
    esac

    if [ -n "$INDEX" ]; then
        if [ ! -d "$(dirname "$INDEX")" ]; then
            fail "the start page's directory $(dirname "$INDEX") is missing"
        elif [ ! -f "$INDEX" ]; then
            warn "no start page $INDEX yet: run the sync"
        elif [ -z "$(find "$INDEX" -perm -004 2>/dev/null)" ]; then
            fail "start page $INDEX is not readable for nginx"
        elif ! reachable "$INDEX"; then
            fail "nginx cannot reach $INDEX: a directory above it does not" \
                 "let others through (chmod o+x)"
        else
            ok "start page $INDEX"
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
                if [ "${CHECK_DNS-}" = no ]; then
                    ok "DNS not checked (CHECK_DNS=no)"
                    break
                fi
                a=$(getent hosts "$h" 2>/dev/null | awk '{print $1; exit}')
                rec=$( [ "$h" = "$dom" ] && echo "$dom" || echo "*.$dom")
                if [ -z "$a" ]; then
                    fail "$h does not resolve: no DNS record for $rec" \
                         "(CHECK_DNS=no skips this, e.g. in a container)"
                elif printf '%s\n' "$mine" | grep -qx "$a"; then
                    ok "$h resolves to this host ($a)"
                else
                    # Behind NAT the public address is not one of ours
                    warn "$h resolves to $a, not an address of this host" \
                         "(fine behind NAT)"
                fi
            done ;;
    esac

    if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
        if systemctl is-active --quiet camex-nginx-sync.service; then
            ok "camex-nginx-sync.service is running"
        else
            warn "camex-nginx-sync.service is not running (systemctl" \
                 "enable --now camex-nginx-sync.service)"
        fi
    fi
    [ "$fails" -eq 0 ] && echo "all good" || echo "$fails problem(s)"
    [ "$fails" -eq 0 ]
}

if [ "${1-}" = "--check" ]; then
    check_all
    exit $?
fi

# --follow [<seconds>] (#130): a sync as soon as the servers' clients
# change (camex --status --wait), and at least every <seconds> for the start
# page's live columns: 10 with INDEX, else 300. Servers before 3.11.0 do
# not know the wait: then every 10 seconds
if [ "${1-}" = "--follow" ]; then
    interval=${2-}
    if [ -z "$interval" ]; then
        interval=300
        [ -n "$INDEX" ] && interval=10
    fi
    case "$interval" in
        ''|*[!0-9]*|0) log "--follow needs a whole number of seconds above 0"
                       exit 2 ;;
    esac
    [ "$interval" -le 3600 ] || interval=3600
    set --
    [ -n "$CONTROL" ] && set -- --control "$CONTROL"
    polling=
    # The state before the sync: a change while it runs wakes the wait
    token=$("$CAMEX" --status "$@" --wait - 2>/dev/null)
    frc=$?
    while :; do
        sync_once
        if [ "$frc" -eq 0 ]; then
            new=$("$CAMEX" --status "$@" --wait "$token" \
                  --wait-timeout "$interval" 2>/dev/null)
            frc=$?
            if [ "$frc" -eq 0 ]; then
                token=$new
                continue
            fi
        fi
        if [ "$frc" -eq 3 ] && [ -z "$polling" ]; then
            log "a server before camex 3.11.0 cannot report its changes:" \
                "a sync every 10 seconds"
            polling=1
        fi
        sleep 10 || exit 1
        token=$("$CAMEX" --status "$@" --wait - 2>/dev/null)
        frc=$?
    done
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
