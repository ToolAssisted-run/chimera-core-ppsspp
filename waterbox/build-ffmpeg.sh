#!/bin/sh
# Builds PPSSPP's pinned ffmpeg fork twice - for the host reference and for
# the waterbox guest - into waterbox/obj-{native,guest}/ffmpeg.
#
# Determinism is the whole point of the flag set: --disable-asm and
# --disable-runtime-cpudetect (no SIMD dispatch varying by host CPU),
# --disable-pthreads (single-threaded decode), and the same source bytes for
# both flavors. The guest flavor compiles with the miniBox musl toolchain and
# the waterbox code-model flags.
#
# Usage: ./build-ffmpeg.sh [-m <miniBox dir>] [native|guest|both]
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
ff="$here/../extern/ppsspp/ffmpeg"
mb="${MINIBOX_DIR:-$HOME/chimera/extern/miniBox}"
while getopts "m:" opt; do
	case "$opt" in
		m) mb="$OPTARG" ;;
		*) exit 2 ;;
	esac
done
shift $((OPTIND - 1))
what="${1:-both}"

[ -f "$ff/configure" ] || {
	echo "ffmpeg submodule missing; run: git -C $here/../extern/ppsspp submodule update --init --depth 1 ffmpeg" >&2
	exit 1
}

COMMON="
	--disable-shared --enable-static
	--disable-programs --disable-doc
	--disable-avdevice --disable-avfilter --disable-postproc
	--disable-network --disable-filters
	--disable-encoders --disable-muxers
	--disable-asm --disable-yasm --disable-runtime-cpudetect
	--disable-pthreads --disable-w32threads
	--disable-zlib --disable-bzlib --disable-iconv
	--disable-everything
	--enable-decoder=h264 --enable-decoder=mpeg4 --enable-decoder=h263
	--enable-decoder=h263p --enable-decoder=mpeg2video
	--enable-decoder=mjpeg --enable-decoder=mjpegb
	--enable-decoder=aac --enable-decoder=aac_latm
	--enable-decoder=atrac3 --enable-decoder=atrac3p
	--enable-decoder=mp3 --enable-decoder=pcm_s16le --enable-decoder=pcm_s8
	--enable-parser=h264 --enable-parser=mpeg4video --enable-parser=mpegvideo
	--enable-parser=aac --enable-parser=aac_latm --enable-parser=mpegaudio
	--enable-demuxer=h264 --enable-demuxer=h263 --enable-demuxer=m4v
	--enable-demuxer=mpegps --enable-demuxer=mpegvideo --enable-demuxer=avi
	--enable-demuxer=mp3 --enable-demuxer=aac --enable-demuxer=pmp
	--enable-demuxer=oma --enable-demuxer=pcm_s16le --enable-demuxer=pcm_s8
	--enable-demuxer=wav
	--enable-protocol=file
	--arch=x86_64 --target-os=linux
"

build_flavor() {
	flavor="$1"; cc="$2"; cflags="$3"
	bdir="$here/obj-$flavor/ffmpeg-build"
	prefix="$here/obj-$flavor/ffmpeg"
	[ -f "$prefix/lib/libavcodec.a" ] && { echo "ffmpeg ($flavor): already built"; return 0; }
	rm -rf "$bdir"
	mkdir -p "$bdir"
	(
		cd "$bdir"
		# shellcheck disable=SC2086
		"$ff/configure" --prefix="$prefix" --cc="$cc" \
			--extra-cflags="-D__STDC_CONSTANT_MACROS $cflags" \
			--enable-cross-compile \
			$COMMON > configure.log 2>&1 || { tail -30 configure.log >&2; exit 1; }
		make -j"$(nproc)" > make.log 2>&1 || { tail -30 make.log >&2; exit 1; }
		make install > install.log 2>&1
	)
	echo "ffmpeg ($flavor): built -> $prefix"
}

if [ "$what" = "native" ] || [ "$what" = "both" ]; then
	build_flavor native gcc "-O2"
fi

if [ "$what" = "guest" ] || [ "$what" = "both" ]; then
	mb="$(cd "$mb" && pwd)"
	sr="$mb/build/meson-cpp/guest-sysroot"
	[ -f "$sr/lib/musl-gcc.specs" ] || { echo "miniBox guest toolchain missing at $sr" >&2; exit 1; }
	build_flavor guest "gcc -specs $sr/lib/musl-gcc.specs" \
		"-O2 -fvisibility=hidden -mcmodel=large -mstack-protector-guard=global -fno-pic -fno-pie -fcf-protection=none"
fi
