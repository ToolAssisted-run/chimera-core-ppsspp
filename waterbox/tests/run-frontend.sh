#!/bin/bash
# The frontend half of the gate: load the PPSSPP package in Chimera (under Mono,
# on a private Xvfb display), emulate a fixed number of frames with nothing
# pressed, and require a 64KB slice of user RAM to be byte-identical to the
# native reference run. Then start once from a config that has never seen this
# controller and require the bindings the package ships to become the defaults.
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
[ ${#roms[@]} -eq 0 ] && roms=("$wb/../extern/ppsspp/pspautotests/tests/gpu/triangle/triangle.prx")

if [ -z "$chimera_root" ]; then
	for candidate in "$wb/../../chimera" "$HOME/chimera"; do
		[ -d "$candidate" ] && { chimera_root="$candidate"; break; }
	done
fi
[ -n "$chimera_root" ] && [ -d "$chimera_root" ] || {
	echo "chimera checkout not found; pass --chimera-root <path>" >&2; exit 1; }
chimera_root="$(cd "$chimera_root" && pwd)"

emu_exe="$chimera_root/build/Chimera.exe"
package="$chimera_root/build/Cores/ppsspp.zip"
[ -f "$emu_exe" ] || { echo "Chimera not built: $emu_exe" >&2; exit 1; }
[ -f "$package" ] || { echo "package not installed: $package (run ../build-package.sh)" >&2; exit 1; }
natdir="$wb/../build/meson-native"
[ -x "$natdir/run-native" ] || { echo "native reference not built: meson setup build/meson-native && ninja -C build/meson-native" >&2; exit 1; }

work="$here/work"
mkdir -p "$work"

export LD_LIBRARY_PATH="$chimera_root/build/dll:$chimera_root/build:/usr/lib/x86_64-linux-gnu"
export MONO_CRASH_NOFILE=1 MONO_WINFORMS_XIM_STYLE=disabled ALSOFT_DRIVERS=null
xvfb_pid=""
cleanup() { [ -n "$xvfb_pid" ] && kill "$xvfb_pid" 2>/dev/null; }
trap cleanup EXIT
if [ -z "${DISPLAY:-}" ]; then
	command -v Xvfb >/dev/null || { echo "Xvfb not found (apt install xvfb)" >&2; exit 1; }
	for n in 90 91 92 93 94 95 96; do
		if [ ! -e "/tmp/.X11-unix/X$n" ]; then
			Xvfb ":$n" -screen 0 640x480x24 -nolisten tcp & xvfb_pid=$!
			export DISPLAY=":$n"; break
		fi
	done
	sleep 1
fi

config="$work/config.ini"
if [ ! -f "$config" ]; then
	( cd "$chimera_root" && timeout 120 mono "$emu_exe" --headless "--config=$config" \
		"--lua=$here/exit.lua" ) > "$work/bootstrap.log" 2>&1
	[ -f "$config" ] || { echo "config bootstrap failed (see $work/bootstrap.log)" >&2; exit 1; }
fi
sed -i 's/"DispMethod": [0-9]/"DispMethod": 1/' "$config"

ok=0
failed=0
report() { printf "%-28s %-9s %s\n" "$1" "$2" "$3"; case "$2" in PASS) ok=$((ok+1)) ;; *) failed=$((failed+1)) ;; esac; }
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
	( cd "$chimera_root" && MINIHAWK_JOB="$job" timeout 900 mono "$emu_exe" --headless \
		"--config=$cfg" "--core=$package" \
		"--lua=$here/frontend-ram.lua" "$rom" ) > "$work/$tag.log" 2>&1
	[ -f "$work/$tag.meta.txt" ] && grep -q "^status=OK" "$work/$tag.meta.txt"
}

settings_config() { python3 "$here/settings-config.py" "$config" "$1" "$2"; }

for rom in "${roms[@]}"; do
	name="$(basename "$rom")"
	name="${name%.*}"
	if [ ! -f "$rom" ]; then report "$name" SKIP "file not found"; continue; fi

	# The RAM-slice comparison needs the IR interpreter on both sides: the
	# default JIT writes build-specific emuhack opcodes into RAM (the other
	# legs ride the jit default - both of their sides are the guest build).
	settings_config "$work/config.$name.ini" '{"cpuCore": "ir-interpreter"}'

	# --- the machine the frontend builds must be the one the gate signed off on ---
	# The frontend mounts the file under the fixed name "rom", and the filename
	# leaks into the machine (the fake DiscID); boot the native reference from a
	# copy with that exact name so both sides see the same string.
	cp "$rom" "$work/rom"
	if ! "$natdir/run-native" "$work/rom" --frames "$frames" --cpu ir-interpreter \
		--ram-slice 0x800000 0x10000 "$work/$name.native.slice.bin" \
		> "$work/$name.native.txt" 2>"$work/$name.native.err"; then
		report "$name:frontend" FAIL "native runner error: $(head -1 "$work/$name.native.err")"; continue
	fi
	if ! run_frontend "$name.base" "$work/config.$name.ini" "$frames" "$work/$name.base.png"; then
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
	settings_config "$work/config.$name.model.ini" '{"pspModel": "psp-1000"}'
	if run_frontend "$name.model" "$work/config.$name.model.ini" 1; then
		got_ramsize="$(grep '^ramsize=' "$work/$name.model.meta.txt" | cut -d= -f2)"
		if [ "$got_ramsize" = "33554432" ]; then
			report "$name:settings:model" PASS "pspModel=psp-1000 booted a 32MB machine"
		else
			report "$name:settings:model" FAIL "expected a 32MB RAM domain, got '$got_ramsize'"
		fi
	else
		report "$name:settings:model" FAIL "run did not report OK (see tests/work/$name.model.log)"
	fi

	# --- the bindings the package ships must become the frontend's defaults ---
	python3 "$here/forget-controller.py" "$work/config.$name.ini" "$work/config.$name.keys.ini" "PSP Controller"
	if run_frontend "$name.keys" "$work/config.$name.keys.ini" 1; then
		if python3 "$here/check-keybinds.py" "$work/config.$name.keys.ini" \
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
	settings_config "$work/config.fontbase.ini" '{}'
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
	python3 "$here/settings-config.py" "$config" "$work/config.fontfw.ini" '{"fontSource": "sony"}' "$fwmap"
	if run_frontend "fontbase" "$work/config.fontbase.ini" 120 \
		&& run_frontend "fontfw" "$work/config.fontfw.ini" 120; then
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
else
	rm -rf "$work/sd.engine" "$work/sd.box"
	python3 -c "open('$work/sd.movie.txt','w').write(('||    0,    0,............|'+chr(10))*20)"
	( cd "$chimera_root" && LD_LIBRARY_PATH="$chimera_root/build/dll" timeout 600 "$crun" \
		"$package" "$sd" "$work/sd.movie.txt" --export-savedata "$work/sd.engine" ) \
		> "$work/sd.engine.log" 2>&1
	# run-wbx resolves its own libminiboxhost; the frontend's dll dir must not
	# shadow it through the LD_LIBRARY_PATH this script exports
	LD_LIBRARY_PATH="" timeout 600 "$natdir/run-wbx" "$gstdir/core.wbx" "$sd" 20 --plain-rom \
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

echo
echo "$ok ok, $failed failed"
[ "$failed" -eq 0 ]
