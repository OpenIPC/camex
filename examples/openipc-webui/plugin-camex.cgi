#!/usr/bin/haserl
<%in p/common.cgi %>
<%
page_title="Camex"
init=/etc/init.d/S98camex
vars="camex_server camex_port camex_pubkey camex_psk camex_name camex_transport camex_network"

# The name camex takes without --name: kept from its first connection, else
# the MAC of the default route's interface
default_name() {
	local id
	id=$(head -n 1 /tmp/camex-id 2>/dev/null | tr -cd '0-9A-F')
	[ ${#id} -eq 12 ] || id=$(tr -d ':\n' < "/sys/class/net/${network_interface:-eth0}/address" | tr a-f A-F)
	printf '%s' "$id"
}

# Is the value free of what the environment (or the form) would not keep?
plain() {
	case "$1" in
		*[[:cntrl:]]*|" "*|*" "|*"&#"*) return 1 ;;
	esac
	return 0
}

matches() {
	printf '%s' "$1" | grep -qE "^$2\$"
}

check() {
	local v val
	for v in $vars; do
		val=$(t_value "$v")
		plain "$val" || { error="$v: control characters, spaces at the ends and &amp;# are not allowed"; return 1; }
	done
	[ -z "$camex_server" ] || { matches "$camex_server" '[A-Za-z0-9][A-Za-z0-9._-]*' && [ ${#camex_server} -le 63 ]; } ||
		{ error="Server: a host name or an IPv4 address"; return 1; }
	matches "$camex_port" '[0-9]{1,5}' && [ "$camex_port" -ge 1 ] && [ "$camex_port" -le 65535 ] ||
		{ error="Port: 1 to 65535"; return 1; }
	case "$camex_transport" in udp|tcp) ;; *) error="Transport: udp or tcp"; return 1 ;; esac
	[ -z "$camex_pubkey" ] || matches "$camex_pubkey" '[A-Za-z0-9+/]{43}=' ||
		{ error="Server public key: 44 characters of base64"; return 1; }
	[ -z "$camex_name" ] || { matches "$camex_name" '[A-Za-z0-9][A-Za-z0-9._:-]*' && [ ${#camex_name} -le 31 ]; } ||
		{ error="Client name: up to 31 letters, digits and . _ : -"; return 1; }
	[ "$(printf '%s' "$camex_psk" | wc -c)" -le 127 ] || { error="Pre-shared key: up to 127 bytes"; return 1; }
	[ "$(printf '%s' "$camex_network" | wc -c)" -le 63 ] || { error="Network name: up to 63 bytes"; return 1; }
}

# The value of a variable in $env_all
env_get() {
	printf '%s\n' "$env_all" | sed -n "s/^$1=//p" | head -n 1
}

# One pass over the flash for all the variables; a name alone deletes it.
# Nothing is written when nothing has changed
env_write() {
	local v val tmp rc changed=""
	for v in $vars; do
		[ "$(t_value "$v")" = "$(env_get "$v")" ] || changed=1
	done
	[ -n "$changed" ] || return 0
	tmp=$(umask 077; mktemp /tmp/camex-env.XXXXXX) || return 1
	for v in $vars; do
		val=$(t_value "$v")
		if [ -n "$val" ]; then printf '%s %s\n' "$v" "$val"; else printf '%s\n' "$v"; fi
	done > "$tmp"
	fw_setenv -s "$tmp" >/dev/null 2>&1
	rc=$?
	rm -f "$tmp"
	return $rc
}

ready() {
	[ -n "$camex_server" ] && { [ -n "$camex_network" ] || { [ -n "$camex_psk" ] && [ -n "$camex_pubkey" ]; }; }
}

error=""
env_all=$(fw_printenv 2>/dev/null | grep '^camex_')
def_name=$(default_name)

if [ "$REQUEST_METHOD" = "POST" ] && [ "$POST_action" = "reset" ]; then
	for v in $vars; do eval "$v="; done
	$init stop </dev/null >/dev/null 2>&1
	env_write || redirect_to "$SCRIPT_NAME" "danger" "The settings could not be removed"
	redirect_to "$SCRIPT_NAME" "danger" "Camex is down, its settings are removed"
elif [ "$REQUEST_METHOD" = "POST" ]; then
	for v in $vars; do eval "$v=\$POST_$v"; done
	: "${camex_port:=5800}" "${camex_transport:=udp}"
	if check; then
		# Defaults are not written, so they can change with camex
		[ "$camex_port" = 5800 ] && camex_port=""
		[ "$camex_transport" = udp ] && camex_transport=""
		[ "$camex_name" = "$def_name" ] && camex_name=""
		env_write || redirect_to "$SCRIPT_NAME" "danger" "The settings could not be written"
		$init restart </dev/null >/dev/null 2>&1
		sleep 2
		ready || redirect_to "$SCRIPT_NAME" "warning" "Saved. Camex starts with a server and a network name, or a server, its public key and a pre-shared key"
		redirect_to "$SCRIPT_NAME" "success" "Saved, Camex is restarted"
	fi
else
	for v in $vars; do eval "$v=\$(env_get $v)"; done
fi
: "${camex_port:=5800}" "${camex_transport:=udp}" "${camex_name:=$def_name}"

pid=$(cat /var/run/camex.pid 2>/dev/null)
[ "$(cat "/proc/$pid/comm" 2>/dev/null)" = camex ] || pid=""
dev=""
[ -n "$pid" ] && dev=$(sed -n 's/^iff:[[:space:]]*//p' /proc/$pid/fdinfo/* 2>/dev/null | head -n 1)
addr=""
[ -n "$dev" ] && addr=$(ip -4 addr show dev "$dev" 2>/dev/null | sed -n 's/.*inet \([0-9./]*\).*/\1/p' | head -n 1)
%>

<%in p/header.cgi %>

<% [ -n "$error" ] && notice danger "$error" %>

<div class="row g-4">
	<div class="col-12 col-lg-6">
		<div class="card"><div class="card-body">
			<% card_head "Camex" %>
			<p class="small text-secondary">Camex VPN tunnel for secure remote access.</p>
			<% if [ -n "$pid" ]; then %>
				<dl class="small list">
					<% if [ -n "$addr" ]; then %>
						<dt>Status</dt><dd><span class="text-success">Up</span></dd>
						<dt>Address</dt><dd class="text-break"><% esc "$addr" %></dd>
					<% else %>
						<dt>Status</dt><dd><span class="text-warning">Connecting</span></dd>
					<% fi %>
				</dl>
			<% fi %>
			<form action="<%= $SCRIPT_NAME %>" method="post">
				<% field_text "camex_server" "Server" "Host name or IPv4 address" %>
				<% field_text "camex_port" "Port" %>
				<% field_string "camex_transport" "Transport" "$camex_transport" "udp tcp" %>
				<% field_text "camex_network" "Network name" "Is the key of the network. Without it both keys below are needed" %>
				<% field_text "camex_pubkey" "Server public key" %>
				<% field_text "camex_psk" "Pre-shared key" %>
				<% field_text "camex_name" "Client name" "By default the MAC address" %>
				<% button_submit "Save Changes" %>
			</form>
			<form action="<%= $SCRIPT_NAME %>" method="post" class="mt-3">
				<% field_hidden "action" "reset" %>
				<% button_submit "Reset configuration" "danger" %>
			</form>
		</div></div>
	</div>
</div>

<details class="mt-4">
	<summary class="text-secondary small">Advanced — raw configuration</summary>
	<div class="mt-3">
		<%
			ex "fw_printenv | grep ^camex_"
			ex "ps w | grep [/]usr/sbin/camex"
			[ -n "$dev" ] && ex "ip addr show $dev"
			ex "ip route"
			ex "logread | grep camex | tail -n 20"
		%>
	</div>
</details>

<%in p/footer.cgi %>
