#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# Dockerfile — the camex server in Alpine, with the prebuilt static binary
# of a release (the latest one by default). Used by compose.yaml
# (docs/install.md#docker); the files in docker/ are for the tests only.
#
#   docker build -t camex .                          # the latest release
#   docker build --build-arg CAMEX_RELEASE=<url> .   # another one
#

FROM alpine:3.22
ARG CAMEX_RELEASE=https://github.com/OpenIPC/camex/releases/latest/download
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
    if wget -q -O /usr/local/bin/camex "$CAMEX_RELEASE/camex-$A-full" 2>/dev/null; then \
        echo "camex-$A-full"; \
    else \
        wget -q -O /usr/local/bin/camex "$CAMEX_RELEASE/camex-$A"; \
        echo "camex-$A"; \
    fi; \
    chmod 755 /usr/local/bin/camex; \
    camex --version | head -n 1
COPY entrypoint.sh /usr/local/bin/camex-entrypoint
VOLUME /etc/camex
ENTRYPOINT ["/usr/local/bin/camex-entrypoint"]
