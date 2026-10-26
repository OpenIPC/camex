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
#   - the start page's directory, and its password (#119): user camex,
#     the password generated once and kept in /etc/camex/nginx-password
#     (root only), /etc/nginx/camex.htpasswd for nginx; the password is
#     printed at the end of every run;
#   - domain= in [server] of the server's camex.conf, then reloads the
#     running camex servers so they hand it to the sync script.
# Then checks and reloads nginx, enables the timer, runs one sync and
# camex-nginx-sync.sh --check.
#
# Run it again at any time: unchanged files stay, a file that was edited
# since is kept and its difference shown (--force replaces it, keeping the
# old one in /var/backups/camex/). The start page always asks for the
# password: in a kept file only its access lines (auth_basic) are set.
#
# Usage: camex-nginx-setup.sh --domain <domain> [options]
#   --domain <domain>   the cameras' domain: <ID>.<domain>, the start page on
#                       <domain> itself (required)
#   --config <file>     the camex server's config (/etc/camex/camex.conf)
#   --index <file>      the start page (/var/lib/camex-nginx/index.html);
#                       "none": no start page
#   --nginx-conf <file> where the server blocks go
#                       (/etc/nginx/conf.d/camex.conf)
#   --new-password      generate a new password for the start page
#   --force             replace files that differ from what is installed
#   --dry-run           say what would be done, change nothing
#
# Needs root (except --dry-run), nginx, camex, openssl (the password) and
# the files of examples/ next to it.
#

set -u

SRC=$(cd "$(dirname "$0")" && pwd)
DOMAIN=
CONF=/etc/camex/camex.conf
INDEX=/var/lib/camex-nginx/index.html
NGINX_CONF=/etc/nginx/conf.d/camex.conf
MAP=/etc/nginx/camex-map.conf
# The start page's password (#119); the path of the htpasswd file is the
# one in nginx-camex.conf
HTPASSWD=/etc/nginx/camex.htpasswd
PASSWORD_FILE=/etc/camex/nginx-password
BIN=/usr/local/bin
# Backups of replaced files: not next to them, where nginx's
# sites-enabled/* would load a copy too
BACKUP=/var/backups/camex
UNITS=/etc/systemd/system
FORCE=
DRY=
NEWPW=
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
        --new-password) NEWPW=1; shift ;;
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
if [ -n "$INDEX" ] && ! command -v openssl >/dev/null 2>&1; then
    problem "openssl is not installed: it makes the start page's password" \
            "(apt install openssl, dnf install openssl or apk add openssl)"
fi
[ "$PROBLEMS" -eq 0 ] || exit 1

# nginx set up by hand already (docs/nginx.md: nginx-camex.conf): a second
# file with the same names would let nginx pick either, perhaps the one
# without the user's allow/deny/auth_basic. Update that file instead
# (by their real paths: Debian's sites-enabled holds symlinks to
# sites-available, and --nginx-conf may name either)
NGINX_CONF_REAL=$(readlink -f "$NGINX_CONF" 2>/dev/null || echo "$NGINX_CONF")
others=$(for f in $(grep -lsE "^[[:space:]]*include[[:space:]]+$MAP;" \
                    /etc/nginx/conf.d/*.conf /etc/nginx/sites-enabled/* \
                    2>/dev/null); do
             r=$(readlink -f "$f" 2>/dev/null || echo "$f")
             [ "$r" = "$NGINX_CONF_REAL" ] || echo "$f"
         done)
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
    if [ -e "$2" ] && [ ! -f "$2" ]; then
        problem "$2 is not a file (a directory?): left as it is"
        return 1
    fi
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

# put_file <made file> <destination> <mode> [<group>]: through a new file
# next to it, renamed in one step
put_file() {
    nt=
    mkdir -p "$(dirname "$2")" && nt=$(mktemp "$2.XXXXXX") &&
        cp "$1" "$nt" && chmod "$3" "$nt" &&
        { [ -z "${4-}" ] || chgrp "$4" "$nt"; } && mv -f "$nt" "$2" ||
        { [ -n "$nt" ] && rm -f "$nt"; problem "cannot write $2"; return 1; }
}

# nginx checked and reloaded: 0 when it took the configuration
reload_nginx() {
    if ! "$NGINX_BIN" -t >/dev/null 2>&1; then
        problem "nginx -t rejects the configuration:"
        "$NGINX_BIN" -t 2>&1 | sed 's/^/    /' >&2
        return 1
    fi
    if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ] &&
       systemctl is-active --quiet nginx; then
        if systemctl reload nginx; then
            say "reloaded nginx"
            # The new workers take a moment: the check below asks them
            sleep 1
            return 0
        fi
        problem "systemctl reload nginx failed"
    elif "$NGINX_BIN" -s reload 2>/dev/null; then
        say "reloaded nginx"
        sleep 1
        return 0
    else
        problem "nginx did not reload: is it running?"
    fi
    return 1
}

# The group nginx's workers run as: they read the htpasswd file. The
# user directive's group, else its user's group; without the directive
# the user nginx was built with (--user=, nobody by default)
nginx_group() {
    ug=$("$NGINX_BIN" -T 2>/dev/null |
         sed -n 's/^[[:space:]]*user[[:space:]]\{1,\}\([^;#]*\);.*/\1/p' |
         head -n 1)
    # shellcheck disable=SC2086
    set -- $ug
    if [ $# -ge 2 ]; then
        echo "$2"
        return
    fi
    u=${1-}
    if [ -z "$u" ]; then
        u=$("$NGINX_BIN" -V 2>&1 | tr ' ' '\n' | sed -n 's/^--user=//p' | head -n 1)
        [ -n "$u" ] || u=nobody
    fi
    id -gn "$u" 2>/dev/null
}

# The start page's server block of nginx's file $1 (server_name $DOMAIN,
# on one line or more) with the password on: commented auth_basic lines
# taken in, "off" made "camex" (and dropped in its locations), the user
# file ours, missing lines added after its allow/deny lines (or
# server_name), where nginx-camex.conf has them. Everything else stays as
# it is
access_patch() {
    awk -v dom="$DOMAIN" -v pwf="$HTPASSWD" '
        # A line without its comment: from a # outside quotes on
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
        function flush(   i, j, page, at, t, k, ab, uf, ind, out, l, w, names) {
            page = 0
            for (i = 1; i <= nb; i++) {
                if (bd[i] != 1 || code(buf[i]) !~ /^[[:space:]]*server_name([[:space:]]|$)/) {
                    continue
                }
                # The directive up to its ";", over several lines too
                names = ""
                for (j = i; j <= nb; j++) {
                    names = names " " code(buf[j])
                    if (code(buf[j]) ~ /;/) break
                }
                sub(/^[[:space:]]*server_name/, "", names)
                sub(/;.*$/, "", names)
                k = split(names, w, /[[:space:]]+/)
                for (t = 1; t <= k; t++) {
                    if (tolower(w[t]) == dom) {
                        page = i
                    }
                }
            }
            if (!page) {
                for (i = 1; i <= nb; i++) print buf[i]
                nb = 0
                return
            }
            ind = buf[page]
            sub(/[^[:space:]].*$/, "", ind)
            at = page
            for (i = page; i <= nb; i++) {
                if (bd[i] == 1 && code(buf[i]) ~ /;/) {
                    at = i
                    break
                }
            }
            for (i = 1; i <= nb; i++) {
                if (bd[i] == 1 &&
                    buf[i] ~ /^[[:space:]]*#?[[:space:]]*(allow|deny)[[:space:]]/) {
                    at = i
                }
            }
            ab = 0; uf = 0
            for (i = 1; i <= nb; i++) {
                l = buf[i]
                if (bd[i] == 1 &&
                    l ~ /^[[:space:]]*#?[[:space:]]*auth_basic_user_file[[:space:]]/) {
                    if (uf++) continue
                    l = ind "auth_basic_user_file " pwf ";"
                } else if (bd[i] == 1 &&
                           l ~ /^[[:space:]]*#?[[:space:]]*auth_basic[[:space:]]/) {
                    if (ab++) continue
                    sub(/^[[:space:]]*#?[[:space:]]*/, "", l)
                    if (l ~ /^auth_basic[[:space:]]+off[[:space:]]*;/) {
                        l = "auth_basic \"camex\";"
                    }
                    l = ind l
                } else if (bd[i] > 1 &&
                           code(l) ~ /^[[:space:]]*auth_basic[[:space:]]+off[[:space:]]*;[[:space:]]*$/) {
                    # A location would let the page out without it
                    continue
                }
                out[i] = l
            }
            for (i = 1; i <= nb; i++) {
                if (i in out) print out[i]
                if (i == at) {
                    if (!ab) print ind "auth_basic \"camex\";"
                    if (!uf) print ind "auth_basic_user_file " pwf ";"
                }
            }
            nb = 0
        }
        {
            l = code($0)
            o = gsub(/\{/, "{", l)
            c = gsub(/\}/, "}", l)
            if (!inb && depth == 0 && l ~ /^[[:space:]]*server[[:space:]]*\{/) {
                inb = 1
            }
            if (!inb) {
                print
                depth += o - c
                next
            }
            buf[++nb] = $0
            bd[nb] = depth
            depth += o - c
            if (depth <= 0) {
                depth = 0
                inb = 0
                flush()
            }
        }
        END { if (nb) flush() }' "$1"
}

# 0. The start page's password (#119): user camex, always on. The
# password is kept, unless --new-password or there is none yet
PW=
PATCHED=
if [ -n "$INDEX" ]; then
    if [ -z "$NEWPW" ] && [ -f "$PASSWORD_FILE" ]; then
        PW=$(head -n 1 "$PASSWORD_FILE" 2>/dev/null)
    fi
    pw_new=
    if [ -z "$PW" ]; then
        PW=$(openssl rand -hex 10) && [ -n "$PW" ] ||
            { problem "openssl cannot make a password"; exit 1; }
        pw_new=1
    fi
    # The line nginx has for camex, against the password (its salt)
    line=$(grep '^camex:' "$HTPASSWD" 2>/dev/null | head -n 1)
    salt=$(printf '%s\n' "$line" | sed -n 's/^camex:\$apr1\$\([^$]*\)\$.*/\1/p')
    if [ -n "$salt" ] && [ -z "$pw_new" ] &&
       [ "camex:$(printf '%s\n' "$PW" | openssl passwd -apr1 -salt "$salt" -stdin 2>/dev/null)" = "$line" ]; then
        say "unchanged: $HTPASSWD"
        # Its group follows nginx's workers (their user directive may
        # have changed)
        if [ -z "$DRY" ]; then
            grp=$(nginx_group)
            [ -n "$grp" ] && chgrp "$grp" "$HTPASSWD" 2>/dev/null &&
                chmod 640 "$HTPASSWD" 2>/dev/null ||
                problem "cannot give $HTPASSWD mode 640 and the group of" \
                        "nginx's workers (${grp:-none found})"
        fi
    elif [ -n "$DRY" ]; then
        say "would write the start page's password to $HTPASSWD" \
            "${pw_new:+(a new one, kept in $PASSWORD_FILE)}"
    else
        hash=$(printf '%s\n' "$PW" | openssl passwd -apr1 -stdin) && [ -n "$hash" ] ||
            { problem "openssl cannot hash the password"; exit 1; }
        # Other users someone added to the file stay
        { grep -v '^camex:' "$HTPASSWD" 2>/dev/null
          printf 'camex:%s\n' "$hash"; } > "$TMP/htpasswd"
        grp=$(nginx_group)
        if [ -z "$grp" ]; then
            problem "no user for nginx's workers found (user in nginx.conf):" \
                    "$HTPASSWD not written"
            exit 1
        fi
        # nginx's file first: until the new password is saved, the one
        # saved before is the one that does not work, and it is printed
        put_file "$TMP/htpasswd" "$HTPASSWD" 640 "$grp" || exit 1
        say "wrote: $HTPASSWD (user camex)"
        if [ -n "$pw_new" ]; then
            if printf '%s\n' "$PW" > "$TMP/password" &&
               put_file "$TMP/password" "$PASSWORD_FILE" 600; then
                say "wrote: $PASSWORD_FILE (a new password)"
            else
                problem "the start page's password is now $PW, but" \
                        "$PASSWORD_FILE could not keep it"
                exit 1
            fi
        fi
    fi
fi
# Shown at the end of every run
show_login() {
    [ -n "$INDEX" ] && [ -z "$DRY" ] &&
        say "the start page: user camex, password $PW (in $PASSWORD_FILE)"
    return 0
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
# A file there already, edited or of an older version: the start page's
# password goes in first, whatever else differs (#119)
if [ -n "$INDEX" ] && [ -f "$NGINX_CONF_REAL" ]; then
    if ! access_patch "$NGINX_CONF_REAL" > "$TMP/patched"; then
        problem "cannot read $NGINX_CONF_REAL"
        exit 1
    fi
    if ! cmp -s "$TMP/patched" "$NGINX_CONF_REAL"; then
        if [ -n "$DRY" ]; then
            say "would turn on the start page's password in $NGINX_CONF_REAL:"
            diff -u "$NGINX_CONF_REAL" "$TMP/patched" | sed 's/^/    /'
        else
            bak=$BACKUP/$(echo "$NGINX_CONF_REAL" | sed 's|^/||; s|/|_|g').$(date +%Y%m%d-%H%M%S)
            mkdir -p -m 700 "$BACKUP" && cp -p "$NGINX_CONF_REAL" "$bak" ||
                { problem "cannot keep $NGINX_CONF_REAL as $bak"; exit 1; }
            put_file "$TMP/patched" "$NGINX_CONF_REAL" 644 || exit 1
            say "turned on the start page's password in $NGINX_CONF_REAL" \
                "(previous one kept as $bak)"
            PATCHED=1
        fi
    fi
fi
# A symlink (sites-enabled -> sites-available): its target is written
before=$PROBLEMS
install_file "$TMP/nginx.conf" "$NGINX_CONF_REAL" 644
if [ "$PROBLEMS" -ne "$before" ]; then
    # nginx keeps its own file: changing camex's domain now would leave
    # the two on different domains. Its password is on already
    [ -n "$PATCHED" ] && reload_nginx
    say "nothing else changed${PATCHED:+ but the password of the start page}:" \
        "merge the difference into $NGINX_CONF_REAL, or run with --force"
    show_login
    exit 1
fi
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
       grep -qxE "# configuration file ($(printf '%s' "$NGINX_CONF" | sed 's/[.]/\\./g')|$(printf '%s' "$NGINX_CONF_REAL" | sed 's/[.]/\\./g')):"; then
    problem "nginx does not load $NGINX_CONF: put it where nginx.conf" \
            "includes (e.g. /etc/nginx/conf.d/) or include it"
fi
reload_nginx
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
sync_run() {
    env INDEX="$INDEX" MAP="$MAP" CAMEX="$CAMEX" NGINX_BIN="$NGINX_BIN" \
        DOMAIN= CONTROL= DEFAULT= UPSTREAM_PORT= \
        sh "$SRC/camex-nginx-sync.sh" "$@"
}
sync_run || problem "the first sync failed (see above)"
sync_run --check || PROBLEMS=$((PROBLEMS + 1))

show_login
if [ "$PROBLEMS" -eq 0 ]; then
    say "done: the cameras at <ID>.$DOMAIN${INDEX:+, the start page at $DOMAIN}"
    exit 0
fi
say "$PROBLEMS problem(s), see above"
exit 1
