#!/bin/sh
#
# 50-camex.sh — KeeneticOS netfilter hook for camex (issue #33, DRAFT)
#
# NDMS rebuilds iptables on its own (WAN reconnect, settings change) and
# runs every script in /opt/etc/ndm/netfilter.d/ afterwards, with $type
# (iptables / ip6tables) and $table (filter / nat / mangle / raw) set.
# Re-add the rules camex needs each time:
#   - accept traffic arriving from the tunnel;
#   - let it be forwarded to and from the tunnel.
# Not yet validated on a router: rule placement and chain names may need
# adjusting for KeeneticOS.
#

[ "$type" = "ip6tables" ] && exit 0
[ "$table" = "filter" ] || exit 0

CAMEX_IFACE="tun+"
[ -f /opt/etc/camex/camex.env ] && . /opt/etc/camex/camex.env
[ "$ENABLED" = "yes" ] || exit 0

add_rule() {   # add_rule <chain> <match...>
    chain="$1"
    shift
    iptables -C "$chain" "$@" -j ACCEPT 2>/dev/null || \
        iptables -I "$chain" "$@" -j ACCEPT
}

add_rule INPUT   -i "$CAMEX_IFACE"
add_rule FORWARD -i "$CAMEX_IFACE"
add_rule FORWARD -o "$CAMEX_IFACE"

exit 0
