#!/bin/sh
# The equivalence gate: the same PSP program, frame count and per-frame input
# pattern through the native build and through the sandbox, requiring identical
# video/audio/memory-domain digests; then the sandbox again with the whole
# machine round-tripped through save/load state around EVERY frame, requiring
# the digests to come out unchanged.
#
# Usage: ./run-gate.sh [-f frames] [file...]
#   With no files, runs a small default set from pspautotests (free content).
set -u
here="$(cd "$(dirname "$0")" && pwd)"
frames=120
while getopts "f:" opt; do
	case "$opt" in
		f) frames="$OPTARG" ;;
		*) exit 2 ;;
	esac
done
shift $((OPTIND - 1))

tests="$*"
if [ -z "$tests" ]; then
	at="$here/../extern/ppsspp/pspautotests/tests"
	tests="$at/cpu/cpu_alu/cpu_alu.prx $at/gpu/displaylist/state.prx $at/gpu/triangle/triangle.prx $at/threads/mutex/mutex.prx $at/audio/sascore/adsrcurve.prx $at/ctrl/ctrl.prx"
fi

fail=0
for t in $tests; do
	name="$(basename "$t")"
	[ -f "$t" ] || { echo "SKIP $name (missing)"; continue; }
	nat="$("$here/bin/run-native" "$t" --gate --frames "$frames" 2>/dev/null | grep -E '^(videoHash|audioHash|domain\[)')"
	# --axes-via-export: the sandbox run drives the stick through the SetAxis
	# export the way the frontend does; matching the native packed-analog run
	# proves the two input paths land the same machine.
	box="$(timeout 600 "$here/bin/run-wbx" "$here/bin/core.wbx" "$t" "$frames" --axes-via-export 2>/dev/null | grep -E '^(videoHash|audioHash|domain\[)')"
	rr="$(timeout 900 "$here/bin/run-wbx" "$here/bin/core.wbx" "$t" "$frames" --rerecord --axes-via-export 2>/dev/null | grep -E '^(videoHash|audioHash|domain\[)')"
	if [ -z "$nat" ] || [ -z "$box" ]; then
		echo "FAIL $name (a run produced no digests)"; fail=1; continue
	fi
	if [ "$nat" != "$box" ]; then
		echo "FAIL $name (native vs sandbox)"
		echo "--- native"; echo "$nat"; echo "--- sandbox"; echo "$box"
		fail=1; continue
	fi
	if [ "$box" != "$rr" ]; then
		echo "FAIL $name (rerecord diverges)"
		echo "--- plain"; echo "$box"; echo "--- rerecord"; echo "$rr"
		fail=1; continue
	fi
	echo "PASS $name ($frames frames, native==sandbox==rerecord)"
done
exit $fail
