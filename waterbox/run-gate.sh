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
# The default set is content this repository PINS, through the pspautotests
# submodule. A caller who names files may name ones they do not have, and that
# is a SKIP; a default that is not there is a leg this gate has lost - a pin
# bump that moves or renames a path - and it must be a failure, because the
# alternative is what this script used to do: print one SKIP line and exit 0
# with nothing whatever tested.
defaults=0
if [ -z "$tests" ]; then
	defaults=1
	at="$here/../extern/ppsspp/pspautotests/tests"
	tests="$at/cpu/cpu_alu/cpu_alu.prx $at/gpu/displaylist/state.prx $at/gpu/triangle/triangle.prx $at/threads/mutex/mutex.prx $at/audio/sascore/adsrcurve.prx $at/ctrl/ctrl.prx"
fi

fail=0
ran=0
for t in $tests; do
	name="$(basename "$t")"
	if [ ! -f "$t" ]; then
		if [ "$defaults" = 1 ]; then
			echo "FAIL $name (missing: $t - the pinned test set has moved)"; fail=1
		else
			echo "SKIP $name (missing)"
		fi
		continue
	fi
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
	ran=$((ran + 1))
done
if [ "$ran" -eq 0 ]; then
	echo "FAIL gate (not one equivalence leg ran: nothing here was tested)"; fail=1
fi

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
else
	echo "SKIP jit (no $jt - would prove the x86 JIT equals native on everything but RAM, and is deterministic in the guest)"
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
else
	echo "SKIP fonts (no $ft - would prove a font mounted through the firmware channel reaches sceFont)"
fi

# ---- the seeding leg (chimera#161) -----------------------------------------
# A project's save data and DLC go onto the memory stick before the machine
# starts, from the "savedata" and "dlc" slots, each a zip: an entry under PSP/
# where it says (what Export Save Data writes), any other under PSP/SAVEDATA or
# PSP/GAME. idlist.prx looks for its save files before it makes them, so a
# seeded one is found; the stick then holds every seeded file, natively, in
# the sandbox through the slots the frontend mounts, and across a savestate
# around every frame; a zip that is not one, or reaches outside the stick, is
# refused by name.
il="$here/../extern/ppsspp/pspautotests/tests/utility/savedata/idlist.prx"
if [ -f "$il" ]; then
	sdir="$(mktemp -d)"
	python3 - "$sdir" <<'EOF'
import sys, zipfile
d = sys.argv[1]
with zipfile.ZipFile(d + "/save.zip", "w") as z:
    z.writestr("PSP/SAVEDATA/TEST99901/DATA.BIN", bytes(range(16)))   # as Export Save Data writes it
    z.writestr("TEST99901F1/DATA.BIN", bytes(range(16, 32)))           # a save folder, bare
with zipfile.ZipFile(d + "/dlc1.zip", "w") as z:
    z.writestr("ULUS99999/", b"")
    z.writestr("ULUS99999/PARAM.PBP", b"DLCDLC")                       # a game's DLC folder, bare
with zipfile.ZipFile(d + "/dlc2.zip", "w") as z:
    z.writestr("PSP/GAME/NPUH99999/DLC.EDAT", b"EDATEDAT")
with zipfile.ZipFile(d + "/evil.zip", "w") as z:
    z.writestr("../escape.bin", b"x")
open(d + "/notzip.zip", "wb").write(b"this is not a zip")
EOF
	printf '{"savedata":["SAVE.ZIP"],"dlc":["DLC1.ZIP","DLC2.ZIP"]}' > "$sdir/slots.json"
	printf '{"savedata":["SAVE.ZIP"]}' > "$sdir/slots1.json"
	# (mktemp's directory has no spaces: the list goes unquoted, this is sh)
	mounts="--firmware slots=$sdir/slots.json --firmware SAVE.ZIP=$sdir/save.zip --firmware DLC1.ZIP=$sdir/dlc1.zip --firmware DLC2.ZIP=$sdir/dlc2.zip"
	seen="$("$natdir/run-native" "$il" --autotest --cpu ir-interpreter --seed-savedata "$sdir/save.zip" 2>&1 | grep -c 'File exists: ms0:/PSP/SAVEDATA/TEST99901\(F1\)\?/DATA.BIN')"
	unseen="$("$natdir/run-native" "$il" --autotest --cpu ir-interpreter 2>&1 | grep -c 'File exists: ms0:/PSP/SAVEDATA/TEST99901\(F1\)\?/DATA.BIN')"
	"$natdir/run-native" "$il" --gate --frames 5 --cpu ir-interpreter --seed-savedata "$sdir/save.zip" --seed-dlc "$sdir/dlc1.zip" --seed-dlc "$sdir/dlc2.zip" --savedata-out "$sdir/native" >/dev/null 2>&1
	timeout 600 "$natdir/run-wbx" "$gstdir/core.wbx" "$il" 5 --axes-via-export --settings "$irset" $mounts --savedata-out "$sdir/box" >/dev/null 2>&1
	timeout 900 "$natdir/run-wbx" "$gstdir/core.wbx" "$il" 5 --rerecord --axes-via-export --settings "$irset" $mounts --savedata-out "$sdir/rr" >/dev/null 2>&1
	tree="$(cd "$sdir/box" 2>/dev/null && find . -type f | sort | tr '\n' ' ')"
	want="./PSP/GAME/NPUH99999/DLC.EDAT ./PSP/GAME/ULUS99999/PARAM.PBP ./PSP/SAVEDATA/TEST99901/DATA.BIN ./PSP/SAVEDATA/TEST99901F1/DATA.BIN "
	refused=""
	for z in evil notzip; do
		timeout 300 "$natdir/run-wbx" "$gstdir/core.wbx" "$il" 1 --settings "$irset" --firmware "slots=$sdir/slots1.json" --firmware "SAVE.ZIP=$sdir/$z.zip" 2>&1 |
			grep -q "Init failed: the save data SAVE.ZIP cannot go on the memory stick: it \(holds '../escape.bin', which is outside the memory stick\|is not a zip\)" && refused="$refused $z"
	done
	if [ "$seen" != 2 ] || [ "$unseen" != 0 ]; then
		echo "FAIL seed (idlist.prx found $seen of the 2 seeded saves; $unseen without them)"; fail=1
	elif [ "$tree" != "$want" ]; then
		echo "FAIL seed (the stick holds [$tree], not [$want])"; fail=1
	elif ! diff -r "$sdir/native" "$sdir/box" >/dev/null 2>&1 || ! diff -r "$sdir/box" "$sdir/rr" >/dev/null 2>&1; then
		echo "FAIL seed (native, sandbox and rerecord sticks differ)"; fail=1
	elif [ "$refused" != " evil notzip" ]; then
		echo "FAIL seed (refused:$refused of evil notzip)"; fail=1
	else
		echo "PASS seed (save data and 2 DLC zips on the stick: idlist.prx finds the saves, 4 files native==sandbox==rerecord; a zip reaching outside the stick and one that is no zip refused)"
	fi
	rm -rf "$sdir"
else
	echo "SKIP seed (no $il - would prove a project's save data and DLC reach the memory stick)"
fi

# ---- a file written in pieces (chimera#212) --------------------------------
# A truncating open erases nothing on a PSP: the file ends where that handle
# last wrote, once it is closed, and what was there before can be had back by
# seeking past it. LittleBigPlanet installs a 4 MiB archive on the stick a
# megabyte at a time, opening it "truncating" for every piece; the stick used
# to erase it each time, the game read back zeros and said its save data was
# corrupt. The first leg needs no game - run-native drives the stick itself.
pieces="$("$natdir/run-native" --stick-truncate-test 2>&1)"
if [ "$pieces" = "stick-truncate: ok" ]; then
	echo "PASS pieces (a file written through truncating opens keeps every piece, and ends where the last one wrote)"
else
	echo "FAIL pieces (the memory stick's truncating open)"
	echo "$pieces" | head -8
	fail=1
fi

# The second is the game, when PPSSPP_LBP names its image (UCUS98744): Cross
# through its two notices, the install, and on into its opening film - the
# archive on the stick has its first megabyte, and digests and the stick's
# files are the same native, sandboxed and re-recorded.
lbp="${PPSSPP_LBP:-}"
if [ -n "$lbp" ] && [ -f "$lbp" ]; then
	ldir="$natdir/.gate-lbp"
	rm -rf "$ldir"
	mkdir -p "$ldir"
	lframes=2100
	awk -v n="$lframes" 'BEGIN { for (f = 0; f < n; f++) printf "||    0,    0,.........%s..|\n", (f >= 300 && f % 120 < 3) ? "X" : "." }' > "$ldir/movie.txt"
	lnat="$("$natdir/run-native" "$lbp" --gate --frames "$lframes" --movie "$ldir/movie.txt" --cpu ir-interpreter --savedata-out "$ldir/native" 2>/dev/null | grep -E '^(videoHash|audioHash|domain\[)')"
	lbox="$(timeout 900 "$natdir/run-wbx" "$gstdir/core.wbx" "$lbp" "$lframes" --movie "$ldir/movie.txt" --axes-via-export --settings "$irset" --savedata-out "$ldir/box" 2>/dev/null | grep -E '^(videoHash|audioHash|domain\[)')"
	lrr="$(timeout 1800 "$natdir/run-wbx" "$gstdir/core.wbx" "$lbp" "$lframes" --rerecord --movie "$ldir/movie.txt" --axes-via-export --settings "$irset" --savedata-out "$ldir/rr" 2>/dev/null | grep -E '^(videoHash|audioHash|domain\[)')"
	arc="$ldir/native/PSP/SAVEDATA/UCUS98744INSTALL/GUI.ARC"
	if [ ! -s "$arc" ]; then
		echo "FAIL lbp (the game installed no archive on the stick in $lframes frames)"; fail=1
	elif cmp -s -n 1048576 "$arc" /dev/zero; then
		echo "FAIL lbp (the installed archive's first megabyte is zeros: the later pieces erased it)"; fail=1
	elif [ -z "$lnat" ] || [ "$lnat" != "$lbox" ]; then
		echo "FAIL lbp (native vs sandbox digests differ)"; fail=1
	elif [ "$lbox" != "$lrr" ]; then
		echo "FAIL lbp (rerecord digests differ)"; fail=1
	elif ! diff -r "$ldir/native" "$ldir/box" >/dev/null 2>&1 || ! diff -r "$ldir/box" "$ldir/rr" >/dev/null 2>&1; then
		echo "FAIL lbp (the stick's files differ between native, sandbox and rerecord)"; fail=1
	else
		echo "PASS lbp (LittleBigPlanet installs its archive whole and plays on, $(find "$ldir/native" -type f | wc -l) files, native==sandbox==rerecord over $lframes frames)"
	fi
	rm -rf "$ldir"
else
	echo "SKIP lbp (PPSSPP_LBP names no image - would prove the game gets past its install, the same in both flavors)"
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
else
	echo "SKIP savedata (no $sd - would prove the memory stick leaves through the savedata channel, identically in both flavors)"
fi

rm -f "$irset"
exit $fail
