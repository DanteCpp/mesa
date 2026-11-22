#!/bin/sh
# Copyright 2026 The Mesa Authors
# SPDX-License-Identifier: MIT
#
# Refresh win/g18_uapi.h from the g18win kernel driver tree. That header is
# the only definition of the Windows render-node ABI (G4-PLAN 3.4): the kernel
# driver, its tools and this transport all compile the same file. Never edit
# win/g18_uapi.h here; change it in g18win and run this script.
#
# usage: sync-g18-uapi.sh <g18win checkout> [--check]
#   --check: exit 1 if the copy here differs from the checkout's header.
set -eu
src=${1:?usage: sync-g18-uapi.sh <g18win checkout> [--check]}
here=$(cd "$(dirname "$0")" && pwd)
hdr="$src/uapi/g18_uapi.h"
[ -f "$hdr" ] || { echo "no $hdr" >&2; exit 2; }
if [ "${2:-}" = "--check" ]; then
	cmp -s "$hdr" "$here/g18_uapi.h" && { echo "g18_uapi.h in sync"; exit 0; }
	echo "g18_uapi.h differs from $hdr" >&2; exit 1
fi
cp "$hdr" "$here/g18_uapi.h"
rev=$(git -C "$src" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
sum=$(shasum -a 256 "$here/g18_uapi.h" 2>/dev/null || sha256sum "$here/g18_uapi.h")
printf 'g18win %s uapi/g18_uapi.h sha256 %s\n' "$rev" "${sum%% *}" > "$here/g18_uapi.h.src"
cat "$here/g18_uapi.h.src"
