#!/bin/sh
# Compiles musl's libm into a static archive for the NATIVE reference build.
# The guest links musl anyway; giving the native build the same math functions
# makes float-heavy decode paths (atrac3+, ffmpeg audio) bit-identical across
# the two builds - glibc's and musl's transcendentals differ in last-ulp cases,
# which is enough to diverge decoded PCM. sqrt is IEEE-exact everywhere; sin,
# cos, pow, exp2 and friends are not.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
mb="${MINIBOX_DIR:-$HOME/chimera/extern/miniBox}"
mu="$mb/extern/musl"
sr="$mb/build/meson-cpp/guest-sysroot"
out="$here/obj-native"
[ -f "$out/libmuslmath.a" ] && { echo "muslmath: already built"; exit 0; }
mkdir -p "$out/muslmath"
INC="-I$mu/src/include -I$mu/src/internal -I$mu/arch/x86_64 -I$mu/arch/generic -I$sr/include"
for f in "$mu"/src/math/*.c; do
	# shellcheck disable=SC2086
	gcc -O2 -std=c99 -fno-builtin -D_XOPEN_SOURCE=700 $INC -c \
		-o "$out/muslmath/$(basename "$f" .c).o" "$f"
done
for f in "$mu"/src/math/x86_64/*.c "$mu"/src/math/x86_64/*.s; do
	[ -f "$f" ] || continue
	# shellcheck disable=SC2086
	gcc -O2 -fno-builtin $INC -c \
		-o "$out/muslmath/x64_$(basename "$f" | sed 's/\.[cs]$//').o" "$f"
done
ar rcs "$out/libmuslmath.a" "$out"/muslmath/*.o
echo "muslmath: built $out/libmuslmath.a"
