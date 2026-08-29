#!/bin/sh
# The equivalence gate: the same PSP program, frame count and per-frame input
# pattern through the native build and through the sandbox, requiring identical
# video/audio/memory-domain digests; then the sandbox again with the whole
# machine round-tripped through save/load state around EVERY frame, requiring
# the digests to come out unchanged.
#
# Usage: ./run-gate.sh [-n <native build dir>] [-g <guest build dir>] [-f frames] [file...]
#   With no files, runs a small default set from pspautotests (free content).
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
natdir="$root/build/meson-native"
gstdir="$root/build/meson-guest"
frames=120
while getopts "n:g:f:" opt; do
	case "$opt" in
		n) natdir="$OPTARG" ;;
		g) gstdir="$OPTARG" ;;
		f) frames="$OPTARG" ;;
		*) exit 2 ;;
	esac
done
shift $((OPTIND - 1))

# Cross-build equality legs pin the IR interpreter EXPLICITLY: under the
# default x86 JIT, emuhack opcodes make native-vs-guest RAM equality
# impossible by construction (see the jit leg), and an equality gate must not
# depend on what the default happens to be. The jit leg covers the default.
[ -x "$natdir/run-native" ] && [ -x "$natdir/run-wbx" ] || {
	echo "native build missing: meson setup build/meson-native && ninja -C build/meson-native" >&2; exit 1; }
[ -f "$gstdir/core.wbx" ] || {
	echo "guest build missing: sh waterbox/setup-guest.sh && ninja -C build/meson-guest core.wbx" >&2; exit 1; }

irset="$natdir/.gate-ir-settings.json"
printf '{"cpuCore":"ir-interpreter"}' > "$irset"

tests="$*"
if [ -z "$tests" ]; then
	at="$here/../extern/ppsspp/pspautotests/tests"
	tests="$at/cpu/cpu_alu/cpu_alu.prx $at/gpu/displaylist/state.prx $at/gpu/triangle/triangle.prx $at/threads/mutex/mutex.prx $at/audio/sascore/adsrcurve.prx $at/ctrl/ctrl.prx"
fi

fail=0
for t in $tests; do
	name="$(basename "$t")"
	[ -f "$t" ] || { echo "SKIP $name (missing)"; continue; }
	nat="$("$natdir/run-native" "$t" --gate --frames "$frames" --cpu ir-interpreter 2>/dev/null | grep -E '^(videoHash|audioHash|domain\[)')"
	# --axes-via-export: the sandbox run drives the stick through the SetAxis
	# export the way the frontend does; matching the native packed-analog run
	# proves the two input paths land the same machine.
	box="$(timeout 600 "$natdir/run-wbx" "$gstdir/core.wbx" "$t" "$frames" --axes-via-export --settings "$irset" 2>/dev/null | grep -E '^(videoHash|audioHash|domain\[)')"
	rr="$(timeout 900 "$natdir/run-wbx" "$gstdir/core.wbx" "$t" "$frames" --rerecord --axes-via-export --settings "$irset" 2>/dev/null | grep -E '^(videoHash|audioHash|domain\[)')"
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
	# Turbo: the first half of the run with the core's picture switched off and
	# the second half back on. What the machine did, what it sounded like and
	# what it drew once drawing resumed must all be untouched - the whole-run
	# video hash cannot match, and is the one line left out.
	tnorm="$(timeout 600 "$natdir/run-wbx" "$gstdir/core.wbx" "$t" "$frames" --axes-via-export --settings "$irset" 2>/dev/null | grep -E '^(tailVideoHash|audioHash|domain\[)')"
	tturbo="$(timeout 600 "$natdir/run-wbx" "$gstdir/core.wbx" "$t" "$frames" --turbo --axes-via-export --settings "$irset" 2>/dev/null | grep -E '^(tailVideoHash|audioHash|domain\[)')"
	if [ "$tnorm" != "$tturbo" ]; then
		echo "FAIL $name (turbo diverges)"
		echo "--- drawn"; echo "$tnorm"; echo "--- turbo"; echo "$tturbo"
		fail=1; continue
	fi
	echo "PASS $name ($frames frames, native==sandbox==rerecord==turbo)"
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
	jnat="$("$natdir/run-native" "$jt" --gate --frames "$frames" --cpu jit 2>/dev/null | grep -E '^(videoHash|audioHash|domain\[)' | grep -v 'domain\[RAM\]')"
	jbox_full="$(timeout 600 "$natdir/run-wbx" "$gstdir/core.wbx" "$jt" "$frames" --settings "$jset" 2>/dev/null | grep -E '^(videoHash|audioHash|domain\[)')"
	jbox="$(printf '%s\n' "$jbox_full" | grep -v 'domain\[RAM\]')"
	jrr="$(timeout 900 "$natdir/run-wbx" "$gstdir/core.wbx" "$jt" "$frames" --settings "$jset" --rerecord 2>/dev/null | grep -E '^(videoHash|audioHash|domain\[)')"
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

# ---- the fonts leg ---------------------------------------------------------
# Real system fonts arrive through the firmware channel: the frontend mounts
# each provided file under its font file name and the guest overlays it over
# the bundled replacement. PPSSPP ships no zh_gb.pgf (its registry entry is
# optional), so providing one grows sceFont's internal list, which
# fontlist.prx prints - a machine-visible proof the mounted bytes were loaded.
# Free content only: the "provided font" is a copy of the bundled ltn0.pgf.
ft="$here/../extern/ppsspp/pspautotests/tests/font/fontlist.prx"
if [ -f "$ft" ]; then
	fdir="$here/../extern/ppsspp/pspautotests/.gate-fonts"
	mkdir -p "$fdir"
	cp "$here/../extern/ppsspp/assets/flash0/font/ltn0.pgf" "$fdir/zh_gb.pgf"
	fbase="$("$natdir/run-native" "$ft" --gate --frames "$frames" --cpu ir-interpreter 2>/dev/null | grep '^domain\[RAM\]')"
	fnat="$("$natdir/run-native" "$ft" --gate --frames "$frames" --cpu ir-interpreter --font-dir "$fdir" 2>/dev/null | grep -E '^(videoHash|audioHash|domain\[)')"
	fbox="$(timeout 600 "$natdir/run-wbx" "$gstdir/core.wbx" "$ft" "$frames" --axes-via-export --settings "$irset" --firmware "zh_gb.pgf=$fdir/zh_gb.pgf" 2>/dev/null | grep -E '^(videoHash|audioHash|domain\[)')"
	frr="$(timeout 900 "$natdir/run-wbx" "$gstdir/core.wbx" "$ft" "$frames" --rerecord --axes-via-export --settings "$irset" --firmware "zh_gb.pgf=$fdir/zh_gb.pgf" 2>/dev/null | grep -E '^(videoHash|audioHash|domain\[)')"
	rm -rf "$fdir"
	fnat_ram="$(printf '%s\n' "$fnat" | grep '^domain\[RAM\]')"
	if [ -z "$fnat" ] || [ -z "$fbox" ]; then
		echo "FAIL fonts (a run produced no digests)"; fail=1
	elif [ "$fbase" = "$fnat_ram" ]; then
		echo "FAIL fonts (provided font did not reach the machine)"; fail=1
	elif [ "$fnat" != "$fbox" ]; then
		echo "FAIL fonts (native vs sandbox with a provided font)"
		echo "--- native"; echo "$fnat"; echo "--- sandbox"; echo "$fbox"
		fail=1
	elif [ "$fbox" != "$frr" ]; then
		echo "FAIL fonts (rerecord diverges with a provided font)"
		echo "--- plain"; echo "$fbox"; echo "--- rerecord"; echo "$frr"
		fail=1
	else
		echo "PASS fonts (fontlist.prx, provided font shapes the machine, native==sandbox==rerecord)"
	fi
fi

# ---- the savedata leg ------------------------------------------------------
# The memory stick is this core's save data (chimera docs/save-data.md), and
# the savedata guest ABI group is the user's way out. makedata.prx creates a
# savedata directory through the sceUtility dialog and then cleans it up; at
# frame 20 - a pinned count, deterministic in both builds - the files exist,
# so the export must contain them, and native, sandbox and rerecord exports
# must be byte-identical trees.
sd="$here/../extern/ppsspp/pspautotests/tests/utility/savedata/makedata.prx"
if [ -f "$sd" ]; then
	sdir="$here/../extern/ppsspp/pspautotests/.gate-savedata"
	rm -rf "$sdir"
	mkdir -p "$sdir"
	"$natdir/run-native" "$sd" --gate --frames 20 --cpu ir-interpreter --savedata-out "$sdir/native" >/dev/null 2>&1
	timeout 600 "$natdir/run-wbx" "$gstdir/core.wbx" "$sd" 20 --axes-via-export --settings "$irset" --savedata-out "$sdir/box" >/dev/null 2>&1
	timeout 900 "$natdir/run-wbx" "$gstdir/core.wbx" "$sd" 20 --rerecord --axes-via-export --settings "$irset" --savedata-out "$sdir/rr" >/dev/null 2>&1
	nfiles="$(find "$sdir/native" -type f 2>/dev/null | wc -l)"
	if [ "$nfiles" -eq 0 ]; then
		echo "FAIL savedata (the machine wrote no save data to export)"; fail=1
	elif ! diff -r "$sdir/native" "$sdir/box" >/dev/null 2>&1; then
		echo "FAIL savedata (native vs sandbox export trees differ)"
		diff -r "$sdir/native" "$sdir/box" 2>&1 | head -10
		fail=1
	elif ! diff -r "$sdir/box" "$sdir/rr" >/dev/null 2>&1; then
		echo "FAIL savedata (rerecord export tree differs)"
		diff -r "$sdir/box" "$sdir/rr" 2>&1 | head -10
		fail=1
	else
		echo "PASS savedata (makedata.prx, $nfiles files, native==sandbox==rerecord trees)"
	fi
	rm -rf "$sdir"
fi

rm -f "$irset"
exit $fail
