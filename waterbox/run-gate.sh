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

# ---- the JIT leg, one test -------------------------------------------------
# Under the x86 JIT, block linking writes cache-offset "emuhack" opcodes into
# PSP RAM, and generated-code sizes differ between the glibc and musl builds -
# so cross-build RAM equality is impossible BY CONSTRUCTION while emulation is
# equivalent. The reproduction contract binds to the guest build alone, so the
# JIT gate is: cross-build equality on video/audio/VRAM/scratchpad, and full
# five-digest determinism (plain == rerecord) within the guest.
jt="$here/../extern/ppsspp/pspautotests/tests/gpu/triangle/triangle.prx"
if [ -f "$jt" ]; then
	jset="$here/../extern/ppsspp/pspautotests/.gate-jit-settings.json"
	printf '{"cpuCore":"jit"}' > "$jset"
	jnat="$("$here/bin/run-native" "$jt" --gate --frames "$frames" --cpu jit 2>/dev/null | grep -E '^(videoHash|audioHash|domain\[)' | grep -v 'domain\[RAM\]')"
	jbox_full="$(timeout 600 "$here/bin/run-wbx" "$here/bin/core.wbx" "$jt" "$frames" --settings "$jset" 2>/dev/null | grep -E '^(videoHash|audioHash|domain\[)')"
	jbox="$(printf '%s\n' "$jbox_full" | grep -v 'domain\[RAM\]')"
	jrr="$(timeout 900 "$here/bin/run-wbx" "$here/bin/core.wbx" "$jt" "$frames" --settings "$jset" --rerecord 2>/dev/null | grep -E '^(videoHash|audioHash|domain\[)')"
	rm -f "$jset"
	if [ -z "$jnat" ] || [ -z "$jbox" ]; then
		echo "FAIL jit (a run produced no digests)"; fail=1
	elif [ "$jnat" != "$jbox" ]; then
		echo "FAIL jit (native vs sandbox, RAM excluded)"
		echo "--- native"; echo "$jnat"; echo "--- sandbox"; echo "$jbox"
		fail=1
	elif [ "$jbox_full" != "$jrr" ]; then
		echo "FAIL jit (rerecord diverges in the guest)"
		echo "--- plain"; echo "$jbox_full"; echo "--- rerecord"; echo "$jrr"
		fail=1
	else
		echo "PASS jit (triangle.prx, cross-build minus RAM + guest rerecord on all digests)"
	fi
fi

exit $fail
