#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# lib-keys.sh — protocol v2 keys for the e2e tests (source it)
#
# gen_server_key <dir> [camex-binary]
#   writes <dir>/server.key (server_key=...) and sets SERVER_PUBKEY;
#   the image copies server.key to /etc/camex/server.key, the server runs
#   with --server-key-file /etc/camex/server.key, clients get
#   --server-pubkey "$SERVER_PUBKEY".
#
gen_server_key() {
    local dir="$1" bin="${2:-$CAMEX_BINARY}" keys
    keys=$("$bin" --genkey) || return 1
    printf '%s\n' "$keys" | sed -n 's/^server_key=/server_key=/p' > "$dir/server.key"
    SERVER_PUBKEY=$(printf '%s\n' "$keys" | sed -n 's/^public_key=//p')
    [ -s "$dir/server.key" ] && [ -n "$SERVER_PUBKEY" ]
}
SERVER_KEY_ARGS="--server-key-file /etc/camex/server.key"

# build_v2tool <dir>: build tests/v2tool (protocol v2 handshakes for the
# tests) and copy it to <dir>/v2tool
build_v2tool() {
    local repo
    repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    make -s -C "$repo/tests" v2tool >/dev/null && cp "$repo/tests/v2tool" "$1/v2tool"
}
