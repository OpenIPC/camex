#!/bin/sh
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# check-version.sh — fail a pull request that changes code or the build
# but not CAMEX_VERSION in src/version.h (#63).  The new version must be
# MAJOR.MINOR.PATCH without leading zeros, greater than the base one,
# and have its section in CHANGELOG.md ("## <version> — <date>").
# src/, android/, the Makefile, ci/ and scripts/ count as code or build;
# docs, tests, screenshots and the workflows in .github/ do not need a new
# version.  Same script as OpenIPC/vectis, other paths and version macro.
#
# Usage: .github/check-version.sh [BASE]   (default origin/master)
#

set -eu

base=${1:-origin/master}
code="src android Makefile ci scripts"

ver() { sed -n 's/^#define CAMEX_VERSION *"\(.*\)"/\1/p'; }

new=$(ver < src/version.h)
old=$(git show "$base:src/version.h" | ver)

if ! echo "$new" | grep -Eq '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$'; then
	echo "::error::CAMEX_VERSION \"$new\" is not MAJOR.MINOR.PATCH"
	exit 1
fi

# shellcheck disable=SC2086 # $code is a list of paths
changed=$(git diff --name-only "$base"...HEAD -- $code)

if [ "$new" = "$old" ]; then
	if [ -n "$changed" ]; then
		echo "::error::code or build changed, CAMEX_VERSION is still $old:" \
			"bump PATCH (fix) or MINOR (feature) and add its CHANGELOG.md section"
		echo "$changed"
		exit 1
	fi
	echo "version $new unchanged, no code or build changes"
	exit 0
fi

top=$(printf '%s\n%s\n' "$old" "$new" | sort -V | tail -n 1)
if [ "$top" != "$new" ]; then
	echo "::error::CAMEX_VERSION $new is not greater than $old"
	exit 1
fi

# $new is digits and dots only (checked above): escape the dots, require a date.
re=$(printf '%s' "$new" | sed 's/\./\\./g')
if ! grep -Eq "^## $re — [0-9]{4}-[0-9]{2}-[0-9]{2}\$" CHANGELOG.md; then
	echo "::error::CHANGELOG.md has no section \"## $new — YYYY-MM-DD\""
	exit 1
fi

# The section must come with this pull request, not be one the base
# branch already has (a version taken from there brings no notes).
if git show "$base:CHANGELOG.md" 2>/dev/null | grep -Eq "^## $re — "; then
	echo "::error::the section \"## $new\" is already on $base: take a new version"
	exit 1
fi

echo "version $old -> $new, CHANGELOG.md section found"
