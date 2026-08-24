# Native (host) build of the curated PPSSPP source set + the chimera driver.
# This is the REFERENCE build: the same list and defines compile for the guest
# (build-core.sh), and the equivalence gate compares the two. Softgpu + IR
# interpreter only; no UI, no HW GPU backends, no networking backends, no
# retro achievements, no shader translation.
#
# Usage: make -f native.mk -j$(nproc) [O=obj-native]

PP   := ../extern/ppsspp
O    ?= obj-native

include sources.mk

INCS := $(PPINCS)

WARN     := -Wno-deprecated-declarations
CFLAGS   := -O2 -g1 $(DEFS) $(INCS) $(WARN)
CXXFLAGS := $(CFLAGS) -std=c++20

all: objs

objs: $(OBJS)

$(O)/git-version.cpp:
	@mkdir -p $(dir $@)
	printf 'const char *PPSSPP_GIT_VERSION = "%s-chimera";\nconst bool PPSSPP_GIT_VERSION_NO_UPDATE = true;\n' \
	  "$$(git -C $(PP) describe --tags --always)" > $@

$(O)/pp/git-version.o: $(O)/git-version.cpp
	@mkdir -p $(dir $@)
	g++ $(CXXFLAGS) -c -o $@ $<

$(O)/pp/%.o: $(PP)/%.cpp
	@mkdir -p $(dir $@)
	g++ $(CXXFLAGS) -c -o $@ $<

$(O)/pp/%.o: $(PP)/%.c
	@mkdir -p $(dir $@)
	gcc $(CFLAGS) -std=gnu11 -c -o $@ $<

$(O)/pp/%.o: $(PP)/%.cc
	@mkdir -p $(dir $@)
	g++ $(CXXFLAGS) -c -o $@ $<

$(O)/stubs/%.o: stubs/%.cpp
	@mkdir -p $(dir $@)
	g++ $(CXXFLAGS) -c -o $@ $<

list:
	@printf '%s\n' $(SRCS)

clean:
	rm -rf $(O)

.PHONY: all objs list clean

# ---- link the native reference driver --------------------------------------
DRIVER_OBJS := $(O)/drv/psp-driver.o $(O)/drv/run-native.o $(O)/drv/memory-assets.o $(O)/drv/ram-filesystem.o $(O)/assets.o

$(O)/assets.s $(O)/assets-table.inc: build-assets.py
	python3 build-assets.py --embed $(PP)/assets $(O)

$(O)/assets.o: $(O)/assets.s
	gcc -c -o $@ $<

$(O)/drv/memory-assets.o: memory-assets.cpp memory-assets.h $(O)/assets-table.inc
	@mkdir -p $(dir $@)
	g++ $(CXXFLAGS) -c -o $@ $<

$(O)/drv/%.o: %.cpp psp-driver.h
	@mkdir -p $(dir $@)
	g++ $(CXXFLAGS) -c -o $@ $<

bin/run-native: $(OBJS) $(DRIVER_OBJS) $(O)/libchdr.a $(O)/lib7zip.a
	@mkdir -p bin
	g++ -o $@ $(OBJS) $(DRIVER_OBJS) $(O)/libchdr.a $(O)/lib7zip.a -lpthread -ldl -lrt

native: bin/run-native
.PHONY: native
