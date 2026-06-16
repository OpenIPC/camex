#!/bin/sh
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# sync-android-version.sh — extract CAMEX_VERSION from version.h and
# update AndroidManifest.xml + build.gradle.kts to match.
#
# Invoked automatically by 'make apk'.
#

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

VERSION_H="${ROOT_DIR}/src/version.h"
MANIFEST="${ROOT_DIR}/android/app/src/main/AndroidManifest.xml"
GRADLE="${ROOT_DIR}/android/app/build.gradle.kts"

# Extract version from version.h
CAMEX_VERSION=$(grep -Po '(?<=CAMEX_VERSION ")[^"]*' "$VERSION_H")
if [ -z "$CAMEX_VERSION" ]; then
    echo "ERROR: Cannot extract CAMEX_VERSION from $VERSION_H" >&2
    exit 1
fi

# Compute versionCode = MAJOR * 10000 + MINOR * 100 + PATCH
MAJOR=$(echo "$CAMEX_VERSION" | cut -d. -f1)
MINOR=$(echo "$CAMEX_VERSION" | cut -d. -f2)
PATCH=$(echo "$CAMEX_VERSION" | cut -d. -f3)

# Validate parts are integers
case "$MAJOR" in ''|*[!0-9]*) echo "ERROR: Invalid MAJOR='$MAJOR'" >&2; exit 1;; esac
case "$MINOR" in ''|*[!0-9]*) echo "ERROR: Invalid MINOR='$MINOR'" >&2; exit 1;; esac
case "$PATCH" in ''|*[!0-9]*) echo "ERROR: Invalid PATCH='$PATCH'" >&2; exit 1;; esac

VERSION_CODE=$((MAJOR * 10000 + MINOR * 100 + PATCH))

echo "version.h  → CAMEX_VERSION = ${CAMEX_VERSION}"
echo "Computed   → versionCode   = ${VERSION_CODE}"

# --- Update AndroidManifest.xml ---
if [ -f "$MANIFEST" ]; then
    echo "Updating $MANIFEST ..."
    sed -i \
        -e "s|android:versionCode=\"[0-9]*\"|android:versionCode=\"${VERSION_CODE}\"|" \
        -e "s|android:versionName=\"[^\"]*\"|android:versionName=\"${CAMEX_VERSION}\"|" \
        "$MANIFEST"
else
    echo "WARNING: $MANIFEST not found, skipping" >&2
fi

# --- Update build.gradle.kts ---
if [ -f "$GRADLE" ]; then
    echo "Updating $GRADLE ..."
    sed -i \
        -e "s|versionCode = [0-9]*|versionCode = ${VERSION_CODE}|" \
        -e "s|versionName = \"[^\"]*\"|versionName = \"${CAMEX_VERSION}\"|" \
        "$GRADLE"
else
    echo "WARNING: $GRADLE not found, skipping" >&2
fi

echo "✅ Android versions synced to ${CAMEX_VERSION} (versionCode=${VERSION_CODE})"
