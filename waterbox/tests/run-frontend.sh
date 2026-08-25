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
[ -x "$wb/bin/run-native" ] || { echo "native reference not built (run ../build-core.sh)" >&2; exit 1; }

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

	settings_config "$work/config.$name.ini" '{}'

	# --- the machine the frontend builds must be the one the gate signed off on ---
	# The frontend mounts the file under the fixed name "rom", and the filename
	# leaks into the machine (the fake DiscID); boot the native reference from a
	# copy with that exact name so both sides see the same string.
	cp "$rom" "$work/rom"
	if ! "$wb/bin/run-native" "$work/rom" --frames "$frames" \
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

echo
echo "$ok ok, $failed failed"
[ "$failed" -eq 0 ]
