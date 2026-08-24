# Guest (waterbox) build: the same curated source set as native.mk, compiled
# with miniBox's musl/libstdc++ toolchain and the waterbox code-model flags,
# plus the chimera guest ABI adapter, linked into core.wbx.
#
# Prereq: a miniBox checkout built WITH the C++ guest toolchain:
#   meson setup <miniBox>/build/meson-cpp -Dguest_cpp=true && ninja -C <miniBox>/build/meson-cpp
#
# Usage: make -f guest.mk -j$(nproc) [MB=<miniBox dir>]

PP     := ../extern/ppsspp
O      := obj-guest
MB     ?= $(HOME)/chimera/extern/miniBox
MBUILD := $(MB)/build/meson-cpp
SR     := $(MBUILD)/guest-sysroot
GCCVER := $(shell gcc -dumpfullversion)

include sources.mk

WBFLAGS := -fvisibility=hidden -mcmodel=large -mstack-protector-guard=global \
        -fno-pic -fno-pie -fcf-protection=none -O2
SPECS   := -specs $(SR)/lib/musl-gcc.specs
CXXINCS := -I$(SR)/include/c++/$(GCCVER) -I$(SR)/include/c++/$(GCCVER)/x86_64-linux-musl
MBINCS  := -I$(MB)/extern/emulibc -I$(MB)/source/guest/include -I$(MB)/extern/jsmn

WARN     := -Wno-deprecated-declarations
CFLAGS   := $(WBFLAGS) $(DEFS) -DCHIMERA_GUEST $(PPINCS) $(MBINCS) $(WARN)
CXXFLAGS := $(CFLAGS) -std=c++20 -fexceptions $(CXXINCS)

all: objs

objs: $(OBJS) $(O)/libchdr.a $(O)/lib7zip.a $(O)/drv/psp-driver.o $(O)/drv/waterbox.o $(O)/gstubs/guest-syscalls.o $(O)/drv/memory-assets.o $(O)/drv/ram-filesystem.o $(O)/assets.o

$(O)/assets.s $(O)/assets-table.inc: build-assets.py
	python3 build-assets.py --embed $(PP)/assets $(O)

$(O)/assets.o: $(O)/assets.s
	gcc -c -o $@ $<

$(O)/drv/memory-assets.o: memory-assets.cpp memory-assets.h $(O)/assets-table.inc
	@mkdir -p $(dir $@)
	g++ $(SPECS) $(CXXFLAGS) -c -o $@ $<

$(O)/gstubs/%.o: guest-stubs/%.cpp
	@mkdir -p $(dir $@)
	g++ $(SPECS) $(CXXFLAGS) -c -o $@ $<

$(O)/git-version.cpp:
	@mkdir -p $(dir $@)
	printf 'const char *PPSSPP_GIT_VERSION = "%s-chimera";\nconst bool PPSSPP_GIT_VERSION_NO_UPDATE = true;\n' \
	  "$$(git -C $(PP) describe --tags --always)" > $@

$(O)/pp/git-version.o: $(O)/git-version.cpp
	@mkdir -p $(dir $@)
	g++ $(SPECS) $(CXXFLAGS) -c -o $@ $<

$(O)/pp/%.o: $(PP)/%.cpp
	@mkdir -p $(dir $@)
	g++ $(SPECS) $(CXXFLAGS) -c -o $@ $<

$(O)/pp/%.o: $(PP)/%.c
	@mkdir -p $(dir $@)
	gcc $(SPECS) $(CFLAGS) -std=gnu11 -c -o $@ $<

$(O)/pp/%.o: $(PP)/%.cc
	@mkdir -p $(dir $@)
	g++ $(SPECS) $(CXXFLAGS) -c -o $@ $<

$(O)/stubs/%.o: stubs/%.cpp
	@mkdir -p $(dir $@)
	g++ $(SPECS) $(CXXFLAGS) -c -o $@ $<

$(O)/drv/%.o: %.cpp psp-driver.h
	@mkdir -p $(dir $@)
	g++ $(SPECS) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -rf $(O)

.PHONY: all objs clean
