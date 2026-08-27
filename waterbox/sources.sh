#!/bin/sh
# The curated PPSSPP source set: what the Chimera core compiles of upstream, and
# nothing else. Both flavors of the build read it from here - the guest
# (core.wbx) and the native reference the equivalence gate compares against -
# so there is one answer to "which sources are this core", not two that drift.
#
# It is a script rather than a list because the set is described by SHAPE, not
# by enumeration: "all of Core except the websocket debugger", "the compute half
# of Common", "softgpu and what it uses". A pin bump that adds a file to Core
# should be picked up; a pin bump that adds a UI backend should not.
#
# Prints paths relative to the repository root, one per line.
#
# Usage: sources.sh [main|chdr|lzmasdk]
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
pp="extern/ppsspp"
cd "$root"

# find(1) with the exclusions spelled as a grep, so each one can carry its
# reason next to it rather than hiding in a filter-out chain.
core_srcs() {
	find "$pp/Core" \( -name '*.cpp' -o -name '*.c' \) | grep -v \
		-e "^$pp/Core/Debugger/WebSocket" \
		-e "^$pp/Core/RetroAchievements.cpp$" \
		-e "^$pp/Core/Util/PortManager.cpp$" \
		-e "^$pp/Core/MIPS/MIPSAsm.cpp$" \
		-e "^$pp/Core/WebServer.cpp$"
}

# Common: the compute/emu-support subset. No UI, no VR, no GPU backend
# runtimes, no platform text renderers.
common_srcs() {
	ls "$pp"/Common/*.cpp "$pp"/Common/*.c 2>/dev/null | grep -v -e "^$pp/Common/PathBrowser.cpp$"
	for d in Crypto Data File Input Log Math Serialize Thread; do
		find "$pp/Common/$d" \( -name '*.cpp' -o -name '*.c' \)
	done | grep -v -e "^$pp/Common/File/PathBrowser.cpp$"
	for f in \
		GPU/GPUBackendCommon.cpp GPU/Shader.cpp GPU/ShaderWriter.cpp GPU/thin3d.cpp \
		Net/HTTPClient.cpp Net/HTTPHeaders.cpp Net/HTTPRequest.cpp Net/HTTPServer.cpp \
		Net/NetBuffer.cpp Net/Resolve.cpp Net/Sinks.cpp Net/URL.cpp \
		System/Display.cpp System/OSD.cpp System/Request.cpp \
		Render/TextureAtlas.cpp Render/DrawBuffer.cpp Render/Text/draw_text.cpp
	do
		echo "$pp/Common/$f"
	done
}

# GPU: softgpu and the shared machinery it uses.
gpu_srcs() {
	ls "$pp"/GPU/*.cpp 2>/dev/null
	find "$pp/GPU/Common" "$pp/GPU/Software" "$pp/GPU/Debugger" \( -name '*.cpp' -o -name '*.c' \)
}

# ext: the small self-contained vendored libraries the above reference.
ext_srcs() {
	ls "$pp"/ext/zlib/*.c
	ls "$pp"/ext/libpng17/*.c | grep -v -e "^$pp/ext/libpng17/pngtest.c$"
	ls "$pp"/ext/at3_standalone/*.cpp
	ls "$pp"/ext/libkirk/*.c
	echo "$pp/ext/sfmt19937/SFMT.c"
	echo "$pp/ext/cityhash/city.cpp"
	echo "$pp/ext/xxhash.c"
	echo "$pp/ext/gason/gason.cpp"
	echo "$pp/ext/jpge/jpgd.cpp"
	echo "$pp/ext/jpge/jpge.cpp"
	echo "$pp/ext/minimp3/minimp3.cpp"
	ls "$pp"/ext/udis86/*.c
	echo "$pp/ext/disarm.cpp"
	echo "$pp/ext/riscv-disas.cpp"
	echo "$pp/ext/loongarch-disasm.cpp"
	ls "$pp"/ext/snappy/*.cpp
	find "$pp/ext/zstd/lib/common" "$pp/ext/zstd/lib/compress" "$pp/ext/zstd/lib/decompress" -name '*.c'
	echo "$pp/ext/cpu_features/src/impl_x86_linux_or_android.c"
	echo "$pp/ext/cpu_features/src/filesystem.c"
	echo "$pp/ext/cpu_features/src/stack_line_reader.c"
	echo "$pp/ext/cpu_features/src/string_view.c"
	# lua: the library, not its three command-line front ends
	ls "$pp"/ext/lua/*.c | grep -v -e "^$pp/ext/lua/lua.c$" -e "^$pp/ext/lua/luac.c$" -e "^$pp/ext/lua/onelua.c$"
	echo "$pp/ext/aemu_postoffice/client/postoffice.c"
	echo "$pp/ext/aemu_postoffice/client/postoffice_mem_stdc.c"
	echo "$pp/ext/aemu_postoffice/client/mutex_impl_cpp.cpp"
	echo "$pp/ext/aemu_postoffice/client/delay_impl_cpp.cpp"
	echo "$pp/ext/aemu_postoffice/client/log_impl_ppsspp.cpp"
	echo "$pp/ext/aemu_postoffice/client/sock_impl_linux.c"
	echo "$pp/ext/xbrz/xbrz.cpp"
	echo "$pp/ext/basis_universal/basisu_transcoder.cpp"
	# libzip: POSIX only - no Windows crypto, no winzip AES, no UWP randomness
	ls "$pp"/ext/libzip/*.c | grep -v \
		-e "^$pp/ext/libzip/zip_random_uwp.c$" \
		-e "^$pp/ext/libzip/zip_random_win32.c$" \
		-e "^$pp/ext/libzip/zip_source_file_win32" \
		-e "^$pp/ext/libzip/zip_source_winzip_aes" \
		-e "^$pp/ext/libzip/zip_winzip_aes.c$" \
		-e "^$pp/ext/libzip/zip_crypto"
}

# libchdr and the 7z sdk each bundle an LZMA; upstream keeps them apart as two
# static libraries and so do we (same-named symbols resolve from whichever
# archive the linker meets first, matching the CMake build).
chdr_srcs() {
	for f in Alloc.c Bra.c Bra86.c BraIA64.c CpuArch.c Delta.c LzFind.c LzmaDec.c LzmaEnc.c Lzma86Dec.c Sort.c; do
		echo "$pp/ext/libchdr/deps/lzma-24.05/src/$f"
	done
	ls "$pp"/ext/libchdr/src/*.c
}

lzmasdk_srcs() {
	for f in 7zArcIn.c 7zBuf.c 7zCrc.c 7zCrcOpt.c 7zDec.c 7zFile.c 7zStream.c Bcj2.c Bra.c Bra86.c Delta.c Lzma2Dec.c LzmaDec.c; do
		echo "$pp/ext/lzma-sdk/$f"
	done
}

case "${1:-main}" in
	main)
		{ core_srcs; common_srcs; gpu_srcs; ext_srcs; ls waterbox/stubs/*.cpp; } | sort
		;;
	# the lzma the libchdr deps carry, which upstream compiles with the
	# __SWITCH__ workaround; kept apart because only these files get it
	chdr-deps) chdr_srcs | grep '/deps/' | sort ;;
	chdr) chdr_srcs | grep -v '/deps/' | sort ;;
	lzmasdk) lzmasdk_srcs | sort ;;
	*) echo "usage: sources.sh [main|chdr|chdr-deps|lzmasdk]" >&2; exit 2 ;;
esac
