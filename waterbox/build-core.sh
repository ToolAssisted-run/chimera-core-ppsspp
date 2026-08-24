#!/bin/sh
# Builds core.wbx - PPSSPP as a chimera waterbox core - plus run-wbx, the host
# driver the equivalence gate uses.
#
# Prereq: a miniBox checkout built WITH the C++ guest toolchain:
#   meson setup <miniBox>/build/meson-cpp -Dguest_cpp=true
#   ninja -C <miniBox>/build/meson-cpp
#
# Usage: ./build-core.sh [-m <miniBox dir>] [-o <output dir>] [-j N]
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
mb="${MINIBOX_DIR:-$HOME/chimera/extern/miniBox}"
out="$here/bin"
jobs="$(nproc)"
while getopts "m:o:j:" opt; do
	case "$opt" in
		m) mb="$OPTARG" ;;
		o) out="$OPTARG" ;;
		j) jobs="$OPTARG" ;;
		*) exit 2 ;;
	esac
done
mb="$(cd "$mb" && pwd)"
mbuild="$mb/build/meson-cpp"
sr="$mbuild/guest-sysroot"
gccver="$(gcc -dumpfullversion)"

[ -f "$sr/lib/libstdc++.a" ] || {
	echo "miniBox C++ guest toolchain missing at $sr." >&2
	echo "Run: meson setup $mbuild $mb -Dguest_cpp=true && ninja -C $mbuild" >&2
	exit 1
}

# apply the patch series onto the submodule working tree (idempotent)
pp="$here/../extern/ppsspp"
for p in "$here"/../patches/*.patch; do
	[ -f "$p" ] || continue
	if git -C "$pp" apply --check "$p" 2>/dev/null; then
		git -C "$pp" apply "$p"
		echo "applied $(basename "$p")"
	fi
done

# compile the curated source set + adapter for the guest
make -f "$here/guest.mk" -C "$here" -j"$jobs" MB="$mb" objs

mkdir -p "$out"

# The link recipe (library order, --no-relax, the weak pthread pulls) comes
# from miniBox's guest kit; see source/guest/meson.build there.
# libchdr.a before lib7zip.a: their bundled LZMAs overlap and the first archive
# wins the shared symbols, matching upstream's CMake arrangement.
# -z stack-size: musl takes the DEFAULT THREAD stack size from PT_GNU_STACK,
# and its 128KB default overflows under PPSSPP's loader (glibc gives 8MB).
g++ -specs "$sr/lib/musl-gcc.specs" -mcmodel=large -fno-pic -fno-pie \
	-static -no-pie -Wl,--eh-frame-hdr,-O2,--no-relax,-z,stack-size=8388608 -T "$mb/source/guest/linkscript.T" \
	-Wl,-u,pthread_once -Wl,-u,pthread_cond_wait -Wl,-u,pthread_cond_broadcast -Wl,-u,pthread_key_create \
	-o "$out/core.wbx" \
	"$here"/obj-guest/pp/*.o \
	$(find "$here/obj-guest/pp" -mindepth 2 -name '*.o' |  grep -v 'ext/libchdr' | grep -v 'ext/lzma-sdk' | sort) \
	"$here"/obj-guest/stubs/*.o \
	"$here"/obj-guest/drv/psp-driver.o "$here"/obj-guest/drv/waterbox.o \
	"$here"/obj-guest/gstubs/*.o \
	"$mbuild/source/guest/cxxglue.c.o" "$mbuild/source/guest/emulibc.c.o" \
	"$here/obj-guest/libchdr.a" "$here/obj-guest/lib7zip.a" \
	-L"$sr/lib" -lstdc++ -lgcc -lgcc_eh -lc
echo "built $out/core.wbx"

# host driver for the gate
mblinux="$mb/build/meson-linux"
[ -f "$mblinux/source/host/libminiboxhost.so" ] || mblinux="$mbuild"
gcc -O2 -Wall -I"$mb/source/host" -o "$out/run-wbx" "$here/run-wbx.c" \
	"$mblinux/source/host/libminiboxhost.so" -Wl,-rpath,"$mblinux/source/host"
echo "built $out/run-wbx"
