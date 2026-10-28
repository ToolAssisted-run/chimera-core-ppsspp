#!/bin/bash
# The frontend half of the gate: load the PPSSPP package in Chimera's window
# (run headless: no display is needed), emulate a fixed number of frames with
# nothing pressed, and require a 64KB slice of user RAM to be byte-identical to the
# native reference run. Then require the bindings the package ships to be the
# machine's when no key was changed.
#
# Usage:
#   ./run-frontend.sh [--chimera-root <path>] [--frames N] [file...]
set -u

here="$(cd "$(dirname "$0")" && pwd)"
wb="$(cd "$here/.." && pwd)"
frames=120
chimera_root=""
while [ $# -gt 0 ]; do
	case "$1" in
		--chimera-root|--minihawk-root) chimera_root="$2"; shift ;;
		--frames) frames="$2"; shift ;;
		-*) echo "unknown option: $1" >&2; exit 2 ;;
		*) break ;;
	esac
	shift
done

roms=("$@")
pinned=0
[ ${#roms[@]} -eq 0 ] && { pinned=1; roms=("$wb/../extern/ppsspp/pspautotests/tests/gpu/triangle/triangle.prx"); }

if [ -z "$chimera_root" ]; then
	for candidate in "$wb/../../chimera" "$HOME/chimera"; do
		[ -d "$candidate" ] && { chimera_root="$candidate"; break; }
	done
fi
[ -n "$chimera_root" ] && [ -d "$chimera_root" ] || {
	echo "chimera checkout not found; pass --chimera-root <path>" >&2; exit 1; }
chimera_root="$(cd "$chimera_root" && pwd)"

chimera_exe="$chimera_root/build/meson-linux/source/ui/chimera"
package="$chimera_root/build/Cores/ppsspp.chimeraCore"
[ -x "$chimera_exe" ] || { echo "Chimera not built: $chimera_exe" >&2; exit 1; }
[ -f "$package" ] || { echo "package not installed: $package (run ../build-package.sh)" >&2; exit 1; }
natdir="$wb/../build/meson-native"
gstdir="$wb/../build/meson-guest"
[ -x "$natdir/run-native" ] || { echo "native reference not built: meson setup build/meson-native && ninja -C build/meson-native" >&2; exit 1; }

work="$here/work"
mkdir -p "$work"

# Chimera's window, run headless: the cores installed in the checkout, its
# settings, cache and recovery files the run's own, and nothing carried over
# from a Chimera installed on this machine
export ALSOFT_DRIVERS=null CHIMERA_CORES_DIR="$chimera_root/build/Cores" CHIMERA_QT_CACHE="$work/qt-cache" CHIMERA_QT_RECOVERY="$work/qt-recovery" \
	CHIMERA_QT_HASHES="$work/qt-file-hashes.txt" CHIMERA_QT_CHIMERA_SETTINGS="$work/no-chimera-config.ini"

ok=0
failed=0
skipped=0
# SKIP had no case of its own, so it fell through to the default and counted
# as a FAILURE - while the summary printed neither the skips nor any sign that
# a leg had not been asked. The other four cores' frontend gates count and
# print all three; this one does now too.
report() { printf "%-28s %-9s %s\n" "$1" "$2" "$3"; case "$2" in PASS) ok=$((ok+1)) ;; SKIP) skipped=$((skipped+1)) ;; *) failed=$((failed+1)) ;; esac; }
printf "%-28s %-9s %s\n" "Check" "Result" "Detail"
printf "%-28s %-9s %s\n" "-----" "------" "------"

run_frontend() {
	local tag="$1" cfg="$2" nframes="$3" shot="${4:-}"
	local job="$work/job.$tag.txt"
	{
		echo "frames=$nframes"
		echo "out=$work/$tag.ram.bin"
		echo "meta=$work/$tag.meta.txt"
		echo "shot=$shot"
	} > "$job"
	rm -f "$work/$tag.ram.bin" "$work/$tag.meta.txt"
	[ -n "$shot" ] && rm -f "$shot"
	local project
	project="$(game_project "$tag" "$cfg" "$package" "$rom")" || { cp "$work/projects/$tag.make.log" "$work/$tag.log"; return 1; }
	( cd "$chimera_root" && MINIHAWK_JOB="$job" timeout 900 "$chimera_exe" --headless \
		"--config=$cfg" "--core=$package" \
		"--lua=$here/frontend-ram.lua" "--project=$project" ) > "$work/$tag.log" 2>&1
	[ -f "$work/$tag.meta.txt" ] && grep -q "^status=OK" "$work/$tag.meta.txt"
}

# A game runs only in a project (Chimera, 2026-10-26): each run opens one made
# of its game with chimera-run --new-project, holding the settings
# settings_config was given for the config it runs with.
declare -A settings_of firmware_of
# <out settings> <settings JSON> [<firmware JSON>]: the firmware is pinned in the
# run's project as well as remembered in the settings
settings_config() {
	python3 "$here/settings-config.py" "$1" "$2" ${3:+"$3"}
	settings_of["$1"]="$2"
	firmware_of["$1"]="${3:-}"
}
crun="$chimera_root/build/dll/chimera-run"
game_project() { # tag, config, package, game: prints the project's path
	local dir="$work/projects" settings="${settings_of[$2]:-}"
	local pkg="$3" game="$4"
	local fw=() pair
	if [ -n "${firmware_of[$2]:-}" ]; then
		while IFS= read -r pair; do fw+=(--firmware "$pair"); done < <(python3 -c \
			'import json, sys; [print(k + "=" + v) for k, v in json.loads(sys.argv[1]).items()]' "${firmware_of[$2]}")
	fi
	# one copy of each game, every run's project beside it
	local copy="$dir/$(basename "$game")"
	mkdir -p "$dir"
	[ -f "$copy" ] && [ "$(stat -c %s "$copy")" = "$(stat -c %s "$game")" ] || cp "$game" "$copy"
	"$crun" --new-project "$dir/$1.chimeraProject" "$pkg" "$copy" \
		--settings "${settings:-"{}"}" "${fw[@]}" > "$dir/$1.make.log" 2>&1 && echo "$dir/$1.chimeraProject"
}

for rom in "${roms[@]}"; do
	name="$(basename "$rom")"
	name="${name%.*}"
	# The default rom is pinned content (the pspautotests submodule), so its
	# absence is a leg this gate has LOST - a pin bump that moved a path - and
	# not a machine that lacks content. A rom named on the command line is the
	# caller's own and may legitimately not be there.
	if [ ! -f "$rom" ]; then
		if [ "$pinned" = 1 ]; then report "$name" FAIL "missing: $rom - the pinned test program has moved"
		else report "$name" SKIP "file not found: $rom"; fi
		continue
	fi

	# The RAM-slice comparison needs the IR interpreter on both sides: the
	# default JIT writes build-specific emuhack opcodes into RAM (the other
	# legs ride the jit default - both of their sides are the guest build).
	settings_config "$work/settings.$name.json" '{"cpuCore": "ir-interpreter"}'

	# --- the machine the frontend builds must be the one the gate signed off on ---
	# The filename leaks into the machine (PPSSPP invents a DiscID from it for
	# a homebrew image), and the engine mounts a directly-opened rom under its
	# real basename, which is what the guest boots. So the native reference
	# boots the file under that same basename: its own path, unchanged.
	if ! "$natdir/run-native" "$rom" --frames "$frames" --cpu ir-interpreter \
		--ram-slice 0x800000 0x10000 "$work/$name.native.slice.bin" \
		> "$work/$name.native.txt" 2>"$work/$name.native.err"; then
		report "$name:frontend" FAIL "native runner error: $(head -1 "$work/$name.native.err")"; continue
	fi
	if ! run_frontend "$name.base" "$work/settings.$name.json" "$frames" "$work/$name.base.png"; then
		report "$name:frontend" FAIL "no OK meta (see tests/work/$name.base.log)"; continue
	fi
	if cmp -s "$work/$name.native.slice.bin" "$work/$name.base.ram.bin"; then
		report "$name:frontend" PASS "$frames frames, RAM slice identical to the native reference"
	else
		report "$name:frontend" FAIL "RAM slice differs"
	fi

	# --- sync settings must reach the guest ---
	# The PSP model is the crispest machine-shaping setting: a PSP-1000 has
	# 32MB of RAM where the default Slim has 64MB, and the RAM domain's size
	# says which machine actually booted.
	settings_config "$work/settings.$name.model.json" '{"pspModel": "psp-1000"}'
	if run_frontend "$name.model" "$work/settings.$name.model.json" 1; then
		got_ramsize="$(grep '^ramsize=' "$work/$name.model.meta.txt" | cut -d= -f2)"
		if [ "$got_ramsize" = "33554432" ]; then
			report "$name:settings:model" PASS "pspModel=psp-1000 booted a 32MB machine"
		else
			report "$name:settings:model" FAIL "expected a 32MB RAM domain, got '$got_ramsize'"
		fi
	else
		report "$name:settings:model" FAIL "run did not report OK (see tests/work/$name.model.log)"
	fi

	# --- the bindings the package ships are the machine's when nobody changed them
	# (the window keeps only the keys a person changed; fresh settings have none)
	settings_config "$work/settings.$name.keys.json" '{}'
	if run_frontend "$name.keys" "$work/settings.$name.keys.json" 1; then
		printf 'out=%s/%s.controls.json\nmeta=%s/%s.keysdump.txt\n' "$work" "$name" "$work" "$name" > "$work/job.$name.keysdump.txt"
		rm -f "$work/$name.controls.json" "$work/$name.keysdump.txt"
		( cd "$chimera_root" && MINIHAWK_JOB="$work/job.$name.keysdump.txt" timeout 900 "$chimera_exe" --headless \
			"--config=$work/settings.$name.keys.json" "--project=$work/projects/$name.keys.chimeraProject" \
			"--lua=$here/dump-controls.lua" ) >> "$work/$name.keys.log" 2>&1
		if [ ! -f "$work/$name.keysdump.txt" ] || ! grep -q "^status=OK" "$work/$name.keysdump.txt"; then
			report "$name:keybinds" FAIL "the bindings were not written (see tests/work/$name.keys.log)"
		elif python3 "$here/check-keybinds.py" "$work/$name.controls.json" \
			"$wb/default_keybinds.json" "PSP Controller" > "$work/$name.keys.txt" 2>&1; then
			report "$name:keybinds" PASS "$(cat "$work/$name.keys.txt")"
		else
			report "$name:keybinds" FAIL "$(head -1 "$work/$name.keys.txt")"
		fi
	else
		report "$name:keybinds" FAIL "run did not report OK (see tests/work/$name.keys.log)"
	fi
done

# --- a provided system font must reach the guest through the Firmware store ---
# The user points Emulator > Firmware at a real font dump; the frontend mounts
# it under its declared id and sceFont loads it over the bundled replacement.
# PPSSPP ships no zh_gb.pgf, so providing one grows the internal font list -
# fontlist.prx prints that list, and the whole-RAM hash says it changed.
# Free content only: the "dump" is a copy of the bundled ltn0.pgf.
ft="$wb/../extern/ppsspp/pspautotests/tests/font/fontlist.prx"
if [ -f "$ft" ]; then
	rom="$ft"
	cp "$wb/../extern/ppsspp/assets/flash0/font/ltn0.pgf" "$work/zh_gb.pgf"
	settings_config "$work/settings.fontbase.json" '{}'
	# sony fonts: the fontSource setting turns every declared font into a
	# requirement, so the leg provides them all - the bundled files stand in
	# for dumps (free content), the unshipped zh_gb from the copy above
	fwmap="$(python3 - "$wb/waterbox.config" "$wb/../extern/ppsspp/assets/flash0/font" "$work/zh_gb.pgf" <<'PYFW'
import json, os, sys
cfg = json.load(open(sys.argv[1]))
fonts, zh = sys.argv[2], sys.argv[3]
out = {}
for e in cfg.get("firmware", []):
    p = os.path.join(fonts, e["id"])
    out[e["id"]] = p if os.path.exists(p) else zh
print(json.dumps(out))
PYFW
)"
	settings_config "$work/settings.fontfw.json" '{"fontSource": "sony"}' "$fwmap"
	if run_frontend "fontbase" "$work/settings.fontbase.json" 120 \
		&& run_frontend "fontfw" "$work/settings.fontfw.json" 120; then
		h1="$(grep '^ramhash=' "$work/fontbase.meta.txt" | cut -d= -f2)"
		h2="$(grep '^ramhash=' "$work/fontfw.meta.txt" | cut -d= -f2)"
		if [ -n "$h1" ] && [ -n "$h2" ] && [ "$h1" != "$h2" ]; then
			report "fontlist:firmware" PASS "a provided system font reached sceFont (RAM hash changed)"
		else
			report "fontlist:firmware" FAIL "RAM hash did not change (h1=$h1 h2=$h2)"
		fi
	else
		report "fontlist:firmware" FAIL "a run did not report OK (see tests/work/font*.log)"
	fi
else
	report "fontlist:firmware" SKIP "fontlist.prx missing"
fi

# --- the savedata export must survive the whole pipeline ---
# makedata.prx writes savedata files through the sceUtility dialog; at the
# pinned frame 20 they exist (see ../run-gate.sh's savedata leg). Exporting
# through the ENGINE (chimera-run --export-savedata: the same
# ce_session_savedata_* calls the Export Save Data menu item makes) must
# produce the identical tree to the standalone sandbox runner.
sd="$wb/../extern/ppsspp/pspautotests/tests/utility/savedata/makedata.prx"
crun="$chimera_root/build/meson-linux/chimera-run"
if [ ! -f "$sd" ]; then
	report "savedata:engine" SKIP "makedata.prx missing"
elif [ ! -x "$crun" ]; then
	report "savedata:engine" SKIP "chimera-run not built"
elif [ ! -f "$gstdir/core.wbx" ]; then
	# not a SKIP: this leg compares the engine against the standalone sandbox
	# runner, so without the guest build there is nothing to compare against
	report "savedata:engine" FAIL "guest build missing: $gstdir/core.wbx"
else
	rm -rf "$work/sd.engine" "$work/sd.box"
	python3 -c "open('$work/sd.movie.txt','w').write(('||    0,    0,............|'+chr(10))*20)"
	# a game runs only in a project (Chimera, 2026-10-26): one of the program and that movie
	( cd "$chimera_root" && LD_LIBRARY_PATH="$chimera_root/build/dll" "$crun" --new-project "$work/sd.chimeraProject" \
		"$package" "$sd" "$work/sd.movie.txt" ) > "$work/sd.make.log" 2>&1
	( cd "$chimera_root" && LD_LIBRARY_PATH="$chimera_root/build/dll" timeout 600 "$crun" \
		--project "$work/sd.chimeraProject" "$package" --files "$(dirname "$sd")" --export-savedata "$work/sd.engine" ) \
		> "$work/sd.engine.log" 2>&1
	# run-wbx resolves its own libminiboxhost; the frontend's dll dir must not
	# shadow it through the LD_LIBRARY_PATH this script exports. No --plain-rom:
	# the engine boots the rom under its real basename, the DiscID follows the
	# basename, and the savedata tree is named after the DiscID - so the
	# standalone runner has to mount the same name for the trees to be
	# comparable at all.
	LD_LIBRARY_PATH="" timeout 600 "$natdir/run-wbx" "$gstdir/core.wbx" "$sd" 20 \
		--savedata-out "$work/sd.box" > "$work/sd.box.log" 2>&1
	nfiles="$(find "$work/sd.engine" -type f 2>/dev/null | wc -l)"
	if [ "$nfiles" -eq 0 ]; then
		report "savedata:engine" FAIL "engine exported no files (see tests/work/sd.engine.log)"
	elif diff -r "$work/sd.engine" "$work/sd.box" >/dev/null 2>&1; then
		report "savedata:engine" PASS "$nfiles files, engine export tree == sandbox runner tree"
	else
		report "savedata:engine" FAIL "engine vs sandbox export trees differ"
	fi
fi

# A game opened the way every user opens one: through a PROJECT, the file in
# the disc slot, mounted under its own name. Every other leg here and in
# run-gate.sh hands the program over as a rom ("/name", with rom.name beside
# it), and that difference let a core out that could not start a single
# project: upstream's companion-ELF lookup lists the folder the game is in,
# a bare name sits in a folder that opens, and reading it is a system call
# the sandbox does not provide - the guest was killed inside Init
# (chimera#228; patch 0001 takes the lookup out). The leg writes a project
# for the pinned triangle.prx and runs 60 frames of it in the engine.
pj="$wb/../extern/ppsspp/pspautotests/tests/gpu/triangle/triangle.prx"
if [ ! -f "$pj" ]; then
	report "project:boot" FAIL "missing: $pj - the pinned test program has moved"
elif [ ! -x "$crun" ]; then
	report "project:boot" SKIP "chimera-run not built"
else
	rm -rf "$work/project"; mkdir -p "$work/project"
	cp "$pj" "$work/project/triangle.prx"
	python3 - "$work/project" <<'PY'
import hashlib, json, sys
d = sys.argv[1]
sha = hashlib.sha1(open(d + "/triangle.prx", "rb").read()).hexdigest().upper()
keys = "P1 Stick X|P1 Stick Y|P1 Up|P1 Down|P1 Left|P1 Right|P1 Cross|P1 Circle|P1 Square|P1 Triangle|P1 Start|P1 Select|P1 L|P1 R|"
json.dump({
    "title": "project boot", "description": "", "rerecords": 0,
    "core": {"name": "PPSSPP", "version": "", "sha1": ""},
    "files": [{"name": "triangle.prx", "sha1": sha, "slot": "disc"}],
    "settings": {}, "firmware": [], "coreCache": [],
    "headers": {"MovieVersion": "Chimera Project File v1.1", "Platform": "PSP"},
    "inputs": {"columns": [keys.split("|")[:-1]], "entries": ["||    0,    0,............|"] * 60},
    "markers": [], "branches": [],
}, open(d + "/p.chimeraProject", "w"), indent=1)
PY
	( cd "$chimera_root" && LD_LIBRARY_PATH="$chimera_root/build/dll" timeout 600 "$crun" \
		--project "$work/project/p.chimeraProject" "$package" --files "$work/project" --allow-core-mismatch ) \
		> "$work/project.log" 2>&1
	if grep -q '^frames=60' "$work/project.log"; then
		report "project:boot" PASS "a program in a project's disc slot boots and runs 60 frames in the engine"
	else
		report "project:boot" FAIL "$(grep -m1 'miniBox: \|chimera-run: ' "$work/project.log" | cut -c1-110) (see tests/work/project.log)"
	fi
fi

echo
echo "$ok ok, $failed failed, $skipped skipped"
[ "$failed" -eq 0 ]
