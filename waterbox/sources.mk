# The curated PPSSPP source set + defines, shared by the native reference
# build (native.mk) and the guest build (guest.mk). Both must compile exactly
# the same emulation sources.

DEFS := -DNDEBUG -DHEADLESS -DSOFTGPU_ONLY -DHTTPS_NOT_AVAILABLE \
        -DMASKED_PSP_MEMORY -DNO_MMAP \
        -DSTACK_LINE_READER_BUFFER_SIZE=1024 -DSFMT_MEXP=19937 \
        -D_7ZIP_ST -DZ7_ST -DCHIMERA_WATERBOX -DZSTD_DISABLE_ASM=1 -DUSE_FFMPEG=1

PPINCS := -I$(PP) -I$(PP)/Common -I$(PP)/ext -I$(PP)/ext/zlib \
        -I$(PP)/ext/libpng17 -I$(PP)/ext/zstd/lib -I$(PP)/ext/snappy \
        -I$(PP)/ext/cpu_features/include -I$(PP)/ext/libchdr/include \
        -I$(PP)/ext/libchdr/deps/lzma-24.05/include -I$(PP)/ext/rapidjson/include \
        -I$(PP)/ext/libzip -I$(PP)/ext/lzma-sdk \
        -I$(O)

# ---- the curated source list ----------------------------------------------

# Core: everything, minus the websocket debugger and retro achievements.
CORE_SRCS := $(shell find $(PP)/Core -name '*.cpp' -o -name '*.c' | sort)
CORE_SRCS := $(filter-out $(PP)/Core/Debugger/WebSocket%,$(CORE_SRCS))
CORE_SRCS := $(filter-out $(PP)/Core/Debugger/WebSocket.cpp,$(CORE_SRCS))
CORE_SRCS := $(filter-out $(PP)/Core/RetroAchievements.cpp,$(CORE_SRCS))
CORE_SRCS := $(filter-out $(PP)/Core/Util/PortManager.cpp,$(CORE_SRCS))
CORE_SRCS := $(filter-out $(PP)/Core/MIPS/MIPSAsm.cpp,$(CORE_SRCS))
CORE_SRCS := $(filter-out $(PP)/Core/WebServer.cpp,$(CORE_SRCS))

# Common: the compute/emu-support subset. No UI, no VR, no GPU backend
# runtimes, no platform text renderers.
COMMON_DIRS := Crypto Data File Input Log Math Serialize Thread
COMMON_SRCS := $(wildcard $(PP)/Common/*.cpp) $(wildcard $(PP)/Common/*.c) \
        $(foreach d,$(COMMON_DIRS),$(shell find $(PP)/Common/$(d) \( -name '*.cpp' -o -name '*.c' \) | sort)) \
        $(PP)/Common/GPU/GPUBackendCommon.cpp \
        $(PP)/Common/GPU/Shader.cpp \
        $(PP)/Common/GPU/ShaderWriter.cpp \
        $(PP)/Common/GPU/thin3d.cpp \
        $(PP)/Common/Net/HTTPClient.cpp \
        $(PP)/Common/Net/HTTPHeaders.cpp \
        $(PP)/Common/Net/HTTPRequest.cpp \
        $(PP)/Common/Net/HTTPServer.cpp \
        $(PP)/Common/Net/NetBuffer.cpp \
        $(PP)/Common/Net/Resolve.cpp \
        $(PP)/Common/Net/Sinks.cpp \
        $(PP)/Common/Net/URL.cpp \
        $(PP)/Common/System/Display.cpp \
        $(PP)/Common/System/OSD.cpp \
        $(PP)/Common/System/Request.cpp \
        $(PP)/Common/Render/TextureAtlas.cpp \
        $(PP)/Common/Render/DrawBuffer.cpp \
        $(PP)/Common/Render/Text/draw_text.cpp
COMMON_SRCS := $(filter-out $(PP)/Common/File/PathBrowser.cpp,$(COMMON_SRCS))

# GPU: softgpu and the shared machinery it uses.
GPU_SRCS := $(wildcard $(PP)/GPU/*.cpp) \
        $(shell find $(PP)/GPU/Common $(PP)/GPU/Software $(PP)/GPU/Debugger \( -name '*.cpp' -o -name '*.c' \) | sort)

# ext: the small self-contained vendored libraries the above reference.
EXT_SRCS := \
        $(wildcard $(PP)/ext/zlib/*.c) \
        $(filter-out $(PP)/ext/libpng17/pngtest.c,$(wildcard $(PP)/ext/libpng17/*.c)) \
        $(wildcard $(PP)/ext/at3_standalone/*.cpp) \
        $(wildcard $(PP)/ext/libkirk/*.c) \
        $(PP)/ext/sfmt19937/SFMT.c \
        $(PP)/ext/cityhash/city.cpp \
        $(PP)/ext/xxhash.c \
        $(PP)/ext/gason/gason.cpp \
        $(PP)/ext/jpge/jpgd.cpp \
        $(PP)/ext/jpge/jpge.cpp \
        $(PP)/ext/minimp3/minimp3.cpp \
        $(wildcard $(PP)/ext/udis86/*.c) \
        $(PP)/ext/disarm.cpp \
        $(PP)/ext/riscv-disas.cpp \
        $(PP)/ext/loongarch-disasm.cpp \
        $(wildcard $(PP)/ext/snappy/*.cpp) \
        $(shell find $(PP)/ext/zstd/lib/common $(PP)/ext/zstd/lib/compress $(PP)/ext/zstd/lib/decompress -name '*.c' | sort) \
        $(PP)/ext/cpu_features/src/impl_x86_linux_or_android.c \
        $(PP)/ext/cpu_features/src/filesystem.c \
        $(PP)/ext/cpu_features/src/stack_line_reader.c \
        $(PP)/ext/cpu_features/src/string_view.c \
\
        $(filter-out $(PP)/ext/lua/lua.c $(PP)/ext/lua/luac.c $(PP)/ext/lua/onelua.c,$(wildcard $(PP)/ext/lua/*.c)) \
        $(PP)/ext/aemu_postoffice/client/postoffice.c \
        $(PP)/ext/aemu_postoffice/client/postoffice_mem_stdc.c \
        $(PP)/ext/aemu_postoffice/client/mutex_impl_cpp.cpp \
        $(PP)/ext/aemu_postoffice/client/delay_impl_cpp.cpp \
        $(PP)/ext/aemu_postoffice/client/log_impl_ppsspp.cpp \
        $(PP)/ext/aemu_postoffice/client/sock_impl_linux.c \
        $(PP)/ext/xbrz/xbrz.cpp \
        $(PP)/ext/basis_universal/basisu_transcoder.cpp \
        $(filter-out $(PP)/ext/libzip/zip_random_uwp.c $(PP)/ext/libzip/zip_random_win32.c \
            $(PP)/ext/libzip/zip_source_file_win32%.c $(PP)/ext/libzip/zip_source_winzip_aes%.c \
            $(PP)/ext/libzip/zip_winzip_aes.c $(PP)/ext/libzip/zip_crypto%.c \
            ,$(wildcard $(PP)/ext/libzip/*.c)) \
$(NOTHING)

SRCS := $(CORE_SRCS) $(COMMON_SRCS) $(GPU_SRCS) $(EXT_SRCS) \
        $(wildcard stubs/*.cpp) $(O)/git-version.cpp

# libchdr and the 7z sdk each bundle an LZMA; upstream keeps them apart as two
# static libraries and so do we (same-named symbols resolve from whichever
# archive the linker meets first, matching the CMake build).
CHDR_SRCS := $(addprefix $(PP)/ext/libchdr/deps/lzma-24.05/src/,Alloc.c Bra.c Bra86.c BraIA64.c CpuArch.c Delta.c LzFind.c LzmaDec.c LzmaEnc.c Lzma86Dec.c Sort.c) \
        $(wildcard $(PP)/ext/libchdr/src/*.c)
LZMASDK_SRCS := $(addprefix $(PP)/ext/lzma-sdk/,7zArcIn.c 7zBuf.c 7zCrc.c 7zCrcOpt.c 7zDec.c 7zFile.c 7zStream.c Bcj2.c Bra.c Bra86.c Delta.c Lzma2Dec.c LzmaDec.c)
CHDR_OBJS := $(patsubst $(PP)/%.c,$(O)/pp/%.o,$(CHDR_SRCS))
LZMASDK_OBJS := $(patsubst $(PP)/%.c,$(O)/pp/%.o,$(LZMASDK_SRCS))

# libchdr's CpuArch.c reuses the __SWITCH__ workaround, as upstream does.
$(O)/pp/ext/libchdr/deps/%.o: CFLAGS += -D__SWITCH__

$(O)/libchdr.a: $(CHDR_OBJS)
	ar rcs $@ $^
$(O)/lib7zip.a: $(LZMASDK_OBJS)
	ar rcs $@ $^

OBJS := $(patsubst %.cc,%.o,$(patsubst %.c,%.o,$(SRCS:.cpp=.o)))
OBJS := $(patsubst $(PP)/%,$(O)/pp/%,$(patsubst stubs/%,$(O)/stubs/%,$(OBJS)))
OBJS := $(patsubst $(O)/git-version.o,$(O)/pp/git-version.o,$(OBJS))

