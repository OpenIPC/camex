#!/bin/sh
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# camex-nginx-setup.sh — set up nginx in front of camex clients with one
# domain, given once (#106)
#
# Installs, with the domain filled in where it belongs:
#   - nginx-camex.conf as the nginx server blocks (cameras by name, the
#     start page on the bare domain), and an empty map so nginx starts;
#   - camex-nginx-sync.sh, its systemd service and timer;
#   - the start page's directory;
#   - domain= in [server] of the server's camex.conf, then reloads the
#     running camex servers so they hand it to the sync script.
# Then checks and reloads nginx, enables the timer, runs one sync and
# camex-nginx-sync.sh --check.
#
# Run it again at any time: unchanged files stay, a file that was edited
# since is kept and its difference shown (--force replaces it, keeping the
# old one in /var/backups/camex/).
#
# Usage: camex-nginx-setup.sh --domain <domain> [options]
#   --domain <domain>   the cameras' domain: <ID>.<domain>, the start page on
#                       <domain> itself (required)
#   --config <file>     the camex server's config (/etc/camex/camex.conf)
#   --index <file>      the start page (/var/lib/camex-nginx/index.html);
#                       "none": no start page
#   --nginx-conf <file> where the server blocks go
#                       (/etc/nginx/conf.d/camex.conf)
#   --force             replace files that differ from what is installed
#   --dry-run           say what would be done, change nothing
#
# Needs root (except --dry-run), nginx, camex and the files of examples/
# next to it.
#

set -u

SRC=$(cd "$(dirname "$0")" && pwd)
DOMAIN=
CONF=/etc/camex/camex.conf
INDEX=/var/lib/camex-nginx/index.html
NGINX_CONF=/etc/nginx/conf.d/camex.conf
MAP=/etc/nginx/camex-map.conf
BIN=/usr/local/bin
# Backups of replaced files: not next to them, where nginx's
# sites-enabled/* would load a copy too
BACKUP=/var/backups/camex
UNITS=/etc/systemd/system
FORCE=
DRY=
CAMEX=${CAMEX:-camex}
NGINX_BIN=${NGINX_BIN:-nginx}
PROBLEMS=0

say() {
    echo "camex-nginx-setup: $*"
}
problem() {
    echo "camex-nginx-setup: $*" >&2
    PROBLEMS=$((PROBLEMS + 1))
}
usage() {
    sed -n 's/^# \{0,1\}//; /^Usage:/,/^Needs/p' "$0" >&2
    exit 2
}
# Run, or only show it with --dry-run
run() {
    if [ -n "$DRY" ]; then
        echo "  would run: $*"
        return 0
    fi
    "$@"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --domain)     [ $# -ge 2 ] || usage; DOMAIN=$2; shift 2 ;;
        --config)     [ $# -ge 2 ] || usage; CONF=$2; shift 2 ;;
        --index)      [ $# -ge 2 ] || usage; INDEX=$2; shift 2 ;;
        --nginx-conf) [ $# -ge 2 ] || usage; NGINX_CONF=$2; shift 2 ;;
        --force)      FORCE=1; shift ;;
        --dry-run)    DRY=1; shift ;;
        -h|--help)    usage ;;
        *)            echo "camex-nginx-setup: unknown option $1" >&2; usage ;;
    esac
done

# The same rule as camex: host name labels (letters, digits, '-'; not at
# the ends) separated by single dots, up to 253 characters
DOMAIN=$(printf '%s' "$DOMAIN" | tr 'A-Z' 'a-z')
if [ -z "$DOMAIN" ]; then
    echo "camex-nginx-setup: --domain is required" >&2
    usage
fi
if [ ${#DOMAIN} -gt 253 ] || [ "$(printf '%s' "$DOMAIN" | wc -l)" -ne 0 ] ||
   ! printf '%s\n' "$DOMAIN" |
     grep -qE '^([a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?)(\.[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?)*$'; then
    echo "camex-nginx-setup: not a domain: $DOMAIN" >&2
    exit 2
fi
[ "$INDEX" = none ] && INDEX=
# The paths go into sed and nginx's configuration: plain ones only
for v in "$INDEX" "$NGINX_CONF" "$CONF"; do
    case "$v" in
        *[!A-Za-z0-9/._-]*)
            echo "camex-nginx-setup: use a path of letters, digits and" \
                 "/ . _ - only: $v" >&2
            exit 2 ;;
    esac
done
case "$INDEX" in
    ''|/*) ;;
    *) echo "camex-nginx-setup: --index needs an absolute path: $INDEX" >&2
       exit 2 ;;
esac
if [ -z "$DRY" ] && [ "$(id -u)" != 0 ]; then
    echo "camex-nginx-setup: run it as root (or with --dry-run)" >&2
    exit 2
fi
for f in nginx-camex.conf camex-nginx-sync.sh camex-nginx-sync.service \
         camex-nginx-sync.timer; do
    if [ ! -f "$SRC/$f" ]; then
        echo "camex-nginx-setup: $SRC/$f is missing (run it from examples/)" >&2
        exit 2
    fi
done
for p in "$CAMEX" "$NGINX_BIN"; do
    command -v "$p" >/dev/null 2>&1 || problem "$p is not installed"
done
[ "$PROBLEMS" -eq 0 ] || exit 1

# nginx set up by hand already (docs/nginx.md: nginx-camex.conf): a second
# file with the same names would let nginx pick either, perhaps the one
# without the user's allow/deny/auth_basic. Update that file instead
others=$(grep -lsE "^[[:space:]]*include[[:space:]]+$MAP;" \
             /etc/nginx/conf.d/*.conf /etc/nginx/sites-enabled/* 2>/dev/null |
         grep -vxF "$NGINX_CONF")
if [ -n "$others" ]; then
    for f in $others; do
        problem "nginx already has a camex configuration: $f. Run with" \
                "--nginx-conf $f to update it (edits there are kept without" \
                "--force), or remove it"
    done
    exit 1
fi

# The file to change: a symlink's target, so the link stays
if [ -f "$CONF" ]; then
    CONF_REAL=$(readlink -f "$CONF" 2>/dev/null || echo "$CONF")
else
    CONF_REAL=$CONF
fi
# [server], as camex reads it: any case, blanks inside the brackets
SERVER_RE='^[[:space:]]*[[][[:space:]]*[Ss][Ee][Rr][Vv][Ee][Rr][[:space:]]*[]][[:space:]]*$'
server_domain() {
    awk -v re="$SERVER_RE" '/^[[:space:]]*\[/ { s = ($0 ~ re) }
        s && /^[[:space:]]*domain[[:space:]]*=/ {
            sub(/^[^=]*=[[:space:]]*/, ""); sub(/[[:space:]]+$/, ""); d = $0
        }
        END { if (d != "") print tolower(d) }' "$CONF_REAL"   # the last one, as camex
}
# Another domain in camex.conf: stop before nginx is changed, or nginx and
# the map would follow two domains
have=
if [ -f "$CONF_REAL" ]; then
    have=$(server_domain)
    if [ -n "$have" ] && [ "$have" != "$DOMAIN" ] && [ -z "$FORCE" ]; then
        problem "$CONF has domain=$have, not $DOMAIN: nothing changed" \
                "(--force sets $DOMAIN there and everywhere else)"
        exit 1
    fi
fi

TMP=$(mktemp -d) || exit 1
# Interrupted: stop here, nothing after this point runs
trap 'rm -rf "$TMP"' EXIT
trap 'rm -rf "$TMP"; echo "camex-nginx-setup: interrupted" >&2; exit 130' INT TERM HUP

# install <made file> <destination> <mode>: put it there unless the same
# file is there already; one that differs is kept without --force
install_file() {
    if [ -f "$2" ] && cmp -s "$1" "$2"; then
        say "unchanged: $2"
        # The contents are right; the mode may not be (a script that lost
        # its x bit)
        [ -n "$DRY" ] || chmod "$3" "$2" 2>/dev/null ||
            problem "cannot set mode $3 on $2"
        return 0
    fi
    if [ -f "$2" ] && [ -z "$FORCE" ]; then
        problem "kept $2: it differs from what would be installed" \
                "(--force replaces it):"
        diff -u "$2" "$1" | sed 's/^/    /' >&2
        return 0
    fi
    if [ -n "$DRY" ]; then
        say "would write: $2"
        return 0
    fi
    if [ -f "$2" ]; then
        bak=$BACKUP/$(echo "$2" | sed 's|^/||; s|/|_|g').$(date +%Y%m%d-%H%M%S)
        if ! { mkdir -p -m 700 "$BACKUP" && cp -p "$2" "$bak"; }; then
            problem "cannot keep $2 as $bak: $2 left as it is"
            return 1
        fi
        say "previous one kept as $bak"
    fi
    # mktemp: a new file nobody could have put a symlink at
    nt=
    mkdir -p "$(dirname "$2")" && nt=$(mktemp "$2.XXXXXX") &&
        cp "$1" "$nt" && chmod "$3" "$nt" && mv -f "$nt" "$2" ||
        { [ -n "$nt" ] && rm -f "$nt"; problem "cannot write $2"; return 1; }
    say "wrote: $2"
}

# 1. nginx: the example's domain and page directory replaced
idir=$(dirname "${INDEX:-/var/lib/camex-nginx/index.html}")
ifile=$(basename "${INDEX:-/var/lib/camex-nginx/index.html}")
sed -e "s/openipc\.click/$DOMAIN/g" -e "s|/var/lib/camex-nginx|$idir|g" \
    -e "s|try_files /index\.html |try_files /$ifile |" \
    "$SRC/nginx-camex.conf" > "$TMP/nginx.conf" ||
    { problem "cannot make the nginx configuration"; exit 1; }
if [ -z "$INDEX" ]; then
    # No start page: only the cameras' server block
    awk '/^# The start page\. It lists every connected device/ {exit} {print}' \
        "$TMP/nginx.conf" > "$TMP/nginx.conf.2" && mv "$TMP/nginx.conf.2" "$TMP/nginx.conf"
fi
install_file "$TMP/nginx.conf" "$NGINX_CONF" 644
if [ ! -f "$MAP" ]; then
    printf 'map $host $camex_upstream {\n}\n' > "$TMP/map"
    install_file "$TMP/map" "$MAP" 644
fi

# 2. The sync script, its service (INDEX) and timer
cp "$SRC/camex-nginx-sync.sh" "$TMP/sync"
install_file "$TMP/sync" "$BIN/camex-nginx-sync.sh" 755
if [ -n "$INDEX" ]; then
    # StateDirectory= creates a directory right in /var/lib only
    case "$idir" in
        /var/lib/*/*) state= ;;
        /var/lib/?*)  state=${idir#/var/lib/} ;;
        *)            state= ;;
    esac
    sed -e "s|^Environment=INDEX=.*|Environment=INDEX=$INDEX|" \
        -e "s|^StateDirectory=.*|StateDirectory=$state|" \
        "$SRC/camex-nginx-sync.service" | grep -v '^StateDirectory=$' \
        > "$TMP/service"
else
    sed -e 's|^Environment=INDEX=.*|Environment=INDEX=|' \
        -e '/^StateDirectory=/d' "$SRC/camex-nginx-sync.service" > "$TMP/service"
fi
cp "$SRC/camex-nginx-sync.timer" "$TMP/timer"
if [ -d "$UNITS" ]; then
    install_file "$TMP/service" "$UNITS/camex-nginx-sync.service" 644
    install_file "$TMP/timer" "$UNITS/camex-nginx-sync.timer" 644
else
    say "no $UNITS: run the sync from cron or with camex-nginx-sync.sh --loop 10" \
        "(INDEX=$INDEX)"
fi

# 3. The start page's directory
if [ -n "$INDEX" ] && [ ! -d "$idir" ]; then
    run mkdir -p -m 755 "$idir" && say "created $idir"
fi

# 4. domain= in [server] of camex.conf
if [ ! -f "$CONF" ]; then
    problem "no $CONF: add domain=$DOMAIN to [server] of the server's" \
            "config (or set DOMAIN= for the sync script)"
else
    if [ "$have" = "$DOMAIN" ]; then
        say "unchanged: domain=$DOMAIN in $CONF"
    elif [ -n "$have" ] && [ -z "$FORCE" ]; then
        problem "kept domain=$have in $CONF (--force sets $DOMAIN)"
    else
        ct=
        awk -v d="$DOMAIN" -v re="$SERVER_RE" '
            /^[[:space:]]*\[/ {
                if (s && !done) { print "domain=" d; done = 1 }
                s = ($0 ~ re)
                print; next
            }
            s && /^[[:space:]]*domain[[:space:]]*=/ {
                if (!done) { print "domain=" d; done = 1 }
                next
            }
            { print }
            END {
                if (!done && s) { print "domain=" d; done = 1 }
                if (!done) { print ""; print "[server]"; print "domain=" d }
            }' "$CONF_REAL" > "$TMP/camex.conf"
        if [ -n "$DRY" ]; then
            say "would set domain=$DOMAIN in [server] of $CONF"
        # Through a copy next to it (its owner and mode), renamed in one
        # step: a failed write leaves the file as it was
        elif cp -p "$CONF_REAL" "$CONF_REAL.bak" &&
             ct=$(mktemp "$CONF_REAL.XXXXXX") &&
             cp -p "$CONF_REAL" "$ct" &&
             cat "$TMP/camex.conf" > "$ct" &&
             mv -f "$ct" "$CONF_REAL"; then
            say "set domain=$DOMAIN in [server] of $CONF_REAL" \
                "(previous: $CONF_REAL.bak)"
        else
            [ -n "${ct-}" ] && rm -f "$ct"
            problem "cannot write $CONF_REAL (left as it was)"
        fi
    fi
fi

# 5. The running servers read their file again (kill -HUP): the pids
# from camex --status --json
pids=$("$CAMEX" --status --json 2>/dev/null |
       grep -o '"pid":[0-9]*' | cut -d: -f2 | sort -u)
if [ -z "$pids" ]; then
    problem "no camex server answers: start it with --config $CONF"
else
    for p in $pids; do
        if run kill -HUP "$p"; then
            say "reloaded the camex server (pid $p)"
        else
            problem "cannot reload the camex server (pid $p): restart it"
        fi
    done
    [ -n "$DRY" ] || sleep 1
fi

# 6. nginx, the timer, a first sync and the check
if [ -n "$DRY" ]; then
    say "would check and reload nginx, enable the timer, run a sync and --check"
    [ "$PROBLEMS" -eq 0 ] && exit 0
    say "$PROBLEMS problem(s), see above"
    exit 1
fi
# nginx must load the file written here, not only some other one
if [ -z "$DRY" ] && ! "$NGINX_BIN" -T 2>/dev/null |
       grep -qxF "# configuration file $NGINX_CONF:"; then
    problem "nginx does not load $NGINX_CONF: put it where nginx.conf" \
            "includes (e.g. /etc/nginx/conf.d/) or include it"
fi
if ! "$NGINX_BIN" -t >/dev/null 2>&1; then
    problem "nginx -t rejects the configuration:"
    "$NGINX_BIN" -t 2>&1 | sed 's/^/    /' >&2
else
    if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ] &&
       systemctl is-active --quiet nginx; then
        if systemctl reload nginx; then
            say "reloaded nginx"
        else
            problem "systemctl reload nginx failed"
        fi
    elif "$NGINX_BIN" -s reload 2>/dev/null; then
        say "reloaded nginx"
    else
        problem "nginx did not reload: is it running?"
    fi
fi
if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ] &&
   [ -d "$UNITS" ]; then
    systemctl daemon-reload &&
        systemctl enable --now camex-nginx-sync.timer >/dev/null 2>&1 &&
        say "enabled camex-nginx-sync.timer" ||
        problem "cannot enable camex-nginx-sync.timer"
fi
# This version's sync script, also when an older one was kept installed
# (it differs: reported above)
# With the service's settings only: none inherited from this shell
SYNC_ENV="INDEX=$INDEX MAP=$MAP CAMEX=$CAMEX NGINX_BIN=$NGINX_BIN DOMAIN= \
CONTROL= DEFAULT= UPSTREAM_PORT="
env $SYNC_ENV sh "$SRC/camex-nginx-sync.sh" ||
    problem "the first sync failed (see above)"
env $SYNC_ENV sh "$SRC/camex-nginx-sync.sh" --check || PROBLEMS=$((PROBLEMS + 1))

if [ "$PROBLEMS" -eq 0 ]; then
    say "done: the cameras at <ID>.$DOMAIN${INDEX:+, the start page at $DOMAIN}"
    exit 0
fi
say "$PROBLEMS problem(s), see above"
exit 1
