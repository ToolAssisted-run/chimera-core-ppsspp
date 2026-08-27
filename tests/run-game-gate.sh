#!/bin/sh
# The real-game gate: replay an input movie (jaffar .sol) over a game image
# through the native build and through the sandbox, requiring identical
# video/audio/memory digests; then once more with the whole machine
# round-tripped through save/load state around EVERY frame.
#
# Game images are not distributable, so this runs only where one exists
# (tests/roms/ is gitignored). Defaults to the Beta Bloc movie.
#
# Usage: ./run-game-gate.sh [game.iso movie.sol]
set -u
here="$(cd "$(dirname "$0")" && pwd)"
wb="$here/../waterbox"

iso="${1:-$here/roms/Beta Bloc (USA) (PSP) (PSN).iso}"
sol="${2:-$here/betabloc.sol}"

[ -f "$iso" ] || { echo "SKIP: no game image at $iso"; exit 0; }
[ -f "$sol" ] || { echo "no movie at $sol" >&2; exit 1; }
natdir="$wb/../build/meson-native"
gstdir="$wb/../build/meson-guest"
[ -x "$natdir/run-native" ] || { echo "build the native reference first: meson setup build/meson-native && ninja -C build/meson-native" >&2; exit 1; }
[ -f "$gstdir/core.wbx" ] || { echo "build the core first: sh waterbox/setup-guest.sh && ninja -C build/meson-guest core.wbx" >&2; exit 1; }

work="$here/work"
mkdir -p "$work"

filter() { grep -E '^(videoHash|audioHash|domain\[)'; }

# Full-digest equality needs the IR interpreter: the default JIT writes
# build-specific emuhack opcodes into RAM (see run-gate.sh's jit leg).
irset="$work/gate-ir-settings.json"
printf '{"cpuCore":"ir-interpreter"}' > "$irset"

echo "replaying $(basename "$sol") ($(grep -c '^||' "$sol") frames) over $(basename "$iso")..."
"$natdir/run-native" "$iso" --movie "$sol" --gate --cpu ir-interpreter 2>/dev/null | filter > "$work/game.native.txt" &
native_pid=$!
timeout 3600 "$natdir/run-wbx" "$gstdir/core.wbx" "$iso" --movie "$sol" --settings "$irset" 2>/dev/null | filter > "$work/game.box.txt"
wait "$native_pid"

if [ ! -s "$work/game.native.txt" ] || [ ! -s "$work/game.box.txt" ]; then
	echo "FAIL: a run produced no digests"; exit 1
fi
if ! cmp -s "$work/game.native.txt" "$work/game.box.txt"; then
	echo "FAIL: native vs sandbox"
	echo "--- native"; cat "$work/game.native.txt"
	echo "--- sandbox"; cat "$work/game.box.txt"
	exit 1
fi
echo "native == sandbox"

timeout 7200 "$natdir/run-wbx" "$gstdir/core.wbx" "$iso" --movie "$sol" --settings "$irset" --rerecord 2>/dev/null | filter > "$work/game.rr.txt"
if ! cmp -s "$work/game.box.txt" "$work/game.rr.txt"; then
	echo "FAIL: rerecord diverges"
	echo "--- plain"; cat "$work/game.box.txt"
	echo "--- rerecord"; cat "$work/game.rr.txt"
	exit 1
fi
echo "rerecord identical"
echo "PASS: $(basename "$iso") over $(basename "$sol")"
