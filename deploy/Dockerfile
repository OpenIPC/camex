#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# Dockerfile — the camex server in Alpine, with the prebuilt static binary
# of a release (the latest one by default). Used by compose.yaml
# (docs/install.md#docker); the files in docker/ are for the tests only.
#
#   docker build -t camex .                          # the latest release
#   docker build --build-arg CAMEX_RELEASE=<url> \
#       --build-arg CAMEX_SHA256=<sha256> .          # another one
#
# The binary is checked before it runs: against CAMEX_SHA256 when given,
# else against the SHA-256 GitHub gives for that file of the latest release
# (#205). Another CAMEX_RELEASE needs CAMEX_SHA256.
#

FROM alpine:3.22
ARG CAMEX_RELEASE=https://github.com/OpenIPC/camex/releases/latest/download
ARG CAMEX_SHA256=
ARG CAMEX_API=https://api.github.com/repos/OpenIPC/camex/releases/latest
# Set by docker build for the platform: amd64, arm64, arm
ARG TARGETARCH
RUN set -eu; \
    case "${TARGETARCH:-$(uname -m)}" in \
        amd64|x86_64)  A=x86_64 ;; \
        arm64|aarch64) A=aarch64 ;; \
        arm|armv7l|armv6l) A=arm ;; \
        *) echo "no camex build for ${TARGETARCH:-$(uname -m)}" >&2; exit 1 ;; \
    esac; \
    # The full build (TAP networks, --bridge, mesh) where the release has it
    if wget -q -O /tmp/camex "$CAMEX_RELEASE/camex-$A-full" 2>/dev/null; then \
        F=camex-$A-full; \
    else \
        wget -q -O /tmp/camex "$CAMEX_RELEASE/camex-$A"; \
        F=camex-$A; \
    fi; \
    want=$CAMEX_SHA256; \
    if [ -z "$want" ]; then \
        [ "$CAMEX_RELEASE" = https://github.com/OpenIPC/camex/releases/latest/download ] || \
            { echo "CAMEX_RELEASE=$CAMEX_RELEASE needs CAMEX_SHA256 (the file's SHA-256)" >&2; exit 1; }; \
        # The release's own record of the file: its "digest" after its name
        want=$(wget -q -O - "$CAMEX_API" | \
            awk -v n="\"$F\"" '$0 ~ "\"name\": *" n { f = 1 } \
                f && /"digest": *"sha256:/ { sub(/.*"sha256:/, ""); sub(/".*/, ""); print; exit }'); \
        [ -n "$want" ] || { echo "no SHA-256 for $F in $CAMEX_API" >&2; exit 1; }; \
    fi; \
    got=$(sha256sum /tmp/camex | cut -d' ' -f1); \
    [ "$got" = "$want" ] || \
        { echo "$F: SHA-256 $got, not $want: not installed" >&2; exit 1; }; \
    mv /tmp/camex /usr/local/bin/camex; \
    chmod 755 /usr/local/bin/camex; \
    echo "$F, SHA-256 $got"; \
    camex --version | head -n 1
COPY entrypoint.sh /usr/local/bin/camex-entrypoint
VOLUME /etc/camex
ENTRYPOINT ["/usr/local/bin/camex-entrypoint"]
