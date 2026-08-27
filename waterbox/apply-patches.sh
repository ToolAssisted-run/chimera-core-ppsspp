#!/bin/sh
# Overlays the chimera patch set onto the pinned PPSSPP submodule. Idempotent:
# a patch that is already applied is skipped, so configuring twice is harmless.
#
# The submodule pin is pristine upstream; every difference this core needs is a
# file in patches/, which is what keeps "what did we change" answerable.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
pp="$here/../extern/ppsspp"
for p in "$here"/../patches/*.patch; do
	[ -f "$p" ] || continue
	if git -C "$pp" apply --check "$p" 2>/dev/null; then
		git -C "$pp" apply "$p"
		echo "applied $(basename "$p")"
	fi
done
