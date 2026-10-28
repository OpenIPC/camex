#!/bin/bash
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# test-connect-page.sh — the page of a camex link, docs/connect/index.html
# (#146), in jsdom: the button that opens the app, the fields it shows, the
# desktop command.
#
# Needs: Docker (node:24-slim, jsdom from npm in a throwaway container).
#
set -u -o pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# (the files go in through stdin: a temporary directory need not be one the
# Docker daemon sees)
out=$(tar -C "$REPO_DIR" -cf - docs/connect/index.html tests/connect-page/connect-test.js |
    docker run --rm -i -e HOME=/tmp -e npm_config_cache=/tmp/npm node:24-slim sh -c '
        mkdir -p /t && cd /t && tar -xf - && cp docs/connect/index.html tests/connect-page/connect-test.js . &&
        npm install --silent --no-audit --no-fund jsdom@24 >/dev/null 2>&1 &&
        node connect-test.js' 2>&1)
rc=$?
echo "$out" | sed 's/^/   /'
if [ $rc -ne 0 ] || echo "$out" | grep -q "^FAIL"; then
    echo "FAIL: the connect page"
    exit 1
fi
echo "PASS: the connect page opens the app with the link's settings"
