# chimera-core-ppsspp: PPSSPP in miniBox's sandbox, as a Chimera core.
#
#   guest (core.wbx)   sh waterbox/setup-guest.sh
#                      ninja -C build/meson-guest core.wbx
#   native reference   meson setup build/meson-native
#                      ninja -C build/meson-native
#
# Both flavors compile the SAME curated source set (waterbox/sources.sh) with
# the SAME defines, and the equivalence gate compares them frame for frame.
# Softgpu and the IR interpreter only: no UI, no hardware GPU backend, no
# networking backend, no retro achievements, no shader translation.
project('chimera-core-ppsspp', 'c', 'cpp',
  default_options: ['c_std=gnu11', 'cpp_std=c++20',
                    'optimization=2', 'debug=false', 'warning_level=0'])

pp = 'extern/ppsspp'
pp_abs = meson.current_source_dir() / pp
py = find_program('python3')
sh = find_program('sh')

# the submodule pin is pristine upstream; overlay the chimera patch set
# (idempotent - see docs/PLAN.md)
run_command(files('waterbox/apply-patches.sh'), check: true)

# miniBox provides the guest kit headers for the guest and the host library the
# sandbox driver loads.
mb = get_option('minibox_dir')
if mb == ''
  mb_candidate = meson.current_source_dir() / '../chimera/extern/tools/chimera-common-minibox'
  if import('fs').is_dir(mb_candidate)
    mb = mb_candidate
  else
    error('pass -Dminibox_dir=<miniBox checkout> (or keep one at ../chimera/extern/tools/chimera-common-minibox)')
  endif
endif

defs = [
  '-DNDEBUG', '-DHEADLESS', '-DSOFTGPU_ONLY', '-DHTTPS_NOT_AVAILABLE',
  '-DMASKED_PSP_MEMORY', '-DNO_MMAP',
  '-DSTACK_LINE_READER_BUFFER_SIZE=1024', '-DSFMT_MEXP=19937',
  '-D_7ZIP_ST', '-DZ7_ST', '-DCHIMERA_WATERBOX', '-DZSTD_DISABLE_ASM=1',
  '-DUSE_FFMPEG=1',
  '-Wno-deprecated-declarations',
]
add_project_arguments(defs, language: ['c', 'cpp'])

inc = include_directories(
  pp, pp / 'Common', pp / 'ext', pp / 'ext/zlib',
  pp / 'ext/libpng17', pp / 'ext/zstd/lib', pp / 'ext/snappy',
  pp / 'ext/cpu_features/include', pp / 'ext/libchdr/include',
  pp / 'ext/libchdr/deps/lzma-24.05/include', pp / 'ext/rapidjson/include',
  pp / 'ext/libzip', pp / 'ext/lzma-sdk',
  'waterbox',
)

# ---- the curated source set ------------------------------------------------
# One definition, in waterbox/sources.sh, read by both flavors. It is described
# by shape ("all of Core except the websocket debugger"), so a pin bump that
# adds an emulation file is picked up and one that adds a UI backend is not.
sources_sh = files('waterbox/sources.sh')
lister = run_command(sources_sh, 'main', check: true)
core_sources = files(lister.stdout().strip().split('\n'))
chdr_deps_sources = files(run_command(sources_sh, 'chdr-deps', check: true).stdout().strip().split('\n'))
chdr_sources = files(run_command(sources_sh, 'chdr', check: true).stdout().strip().split('\n'))
lzmasdk_sources = files(run_command(sources_sh, 'lzmasdk', check: true).stdout().strip().split('\n'))

# The build stamps upstream's version string in rather than letting PPSSPP go
# looking for a git tree at runtime.
git_version = configure_file(
  output: 'git-version.cpp',
  capture: true,
  command: [sh, '-c',
    'printf \'const char *PPSSPP_GIT_VERSION = "%s-chimera";\\nconst bool PPSSPP_GIT_VERSION_NO_UPDATE = true;\\n\' "$(git -C ' + pp_abs + ' describe --tags --always 2>/dev/null || echo unknown)"'])

# PPSSPP reads its atlas, fonts and shaders from disk; a sandboxed core has no
# disk, so they are assembled into the binary as one blob plus a table.
assets = custom_target('assets',
  output: ['assets.s', 'assets-table.inc'],
  input: files('waterbox/build-assets.py'),
  command: [py, '@INPUT@', '--embed', pp_abs / 'assets', '@OUTDIR@'])

# memory-assets.cpp includes the generated table; this is what makes meson
# generate it first.
assets_table = declare_dependency(sources: [assets[1]])

# ---- ffmpeg ----------------------------------------------------------------
# PPSSPP's pinned fork, decode-only, no asm, no runtime CPU detection and no
# threads - the flags are what make its output a function of the input rather
# than of the host. It has its own configure/make, so it is built once, beside
# the sources, and linked as five static archives.
flavor = meson.is_cross_build() ? 'guest' : 'native'
message('building ffmpeg (' + flavor + ') if it is not already built...')
run_command(files('waterbox/build-ffmpeg.sh'), '-m', mb, flavor, check: true)
ffmpeg_dir = meson.current_source_dir() / 'waterbox/obj-' + flavor + '/ffmpeg'
ffmpeg_libs = []
foreach lib : ['avformat', 'avcodec', 'swscale', 'swresample', 'avutil']
  ffmpeg_libs += ffmpeg_dir / 'lib/lib' + lib + '.a'
endforeach
ffmpeg_inc = include_directories('waterbox/obj-' + flavor + '/ffmpeg/include')

# ---- the vendored archives -------------------------------------------------
# libchdr and the 7z sdk each bundle an LZMA. Upstream keeps them in separate
# archives so the duplicate symbols resolve from whichever the linker meets
# first, and the link order below preserves that.
chdr_deps_lib = static_library('chdr-deps', chdr_deps_sources,
  include_directories: inc,
  c_args: ['-D__SWITCH__'])   # libchdr's CpuArch.c reuses this workaround, as upstream does
chdr_lib = static_library('chdr', chdr_sources, include_directories: inc)
lzmasdk_lib = static_library('lzmasdk', lzmasdk_sources, include_directories: inc)

driver_sources = files(
  'waterbox/psp-driver.cpp',
  'waterbox/memory-assets.cpp',
  'waterbox/ram-filesystem.cpp',
)

if meson.is_cross_build()
  # ---- the guest: core.wbx --------------------------------------------------
  # miniBox's musl + libstdc++ toolchain arrives through the cross file
  # (waterbox/setup-guest.sh). Two things here are PPSSPP's own:
  # -z stack-size, because musl takes the default THREAD stack size from
  # PT_GNU_STACK and its 128KB overflows under PPSSPP's loader (glibc gives
  # 8MB); and the guest syscall stubs, which stand in for what a sandbox has
  # no kernel to provide.
  mbdir = meson.get_external_property('minibox_dir')
  mbuild = meson.get_external_property('minibox_build')
  sr = meson.get_external_property('guest_sysroot')

  executable('core',
    core_sources + driver_sources + [assets[0], git_version] + files(
      'waterbox/waterbox.cpp',
      'waterbox/guest-stubs/guest-syscalls.cpp'),
    name_suffix: 'wbx',
    include_directories: [inc, ffmpeg_inc],
    dependencies: [assets_table],
    link_with: [chdr_deps_lib, chdr_lib, lzmasdk_lib],
    link_args: [
      '-static', '-no-pie',
      '-Wl,--eh-frame-hdr', '-Wl,-O2', '-Wl,--no-relax',
      '-Wl,-z,stack-size=8388608',
      '-T', mbdir / 'source/guest/linkscript.T',
      '-Wl,-u,pthread_once', '-Wl,-u,pthread_cond_wait',
      '-Wl,-u,pthread_cond_broadcast', '-Wl,-u,pthread_key_create',
      mbuild / 'source/guest/cxxglue.c.o',
      mbuild / 'source/guest/emulibc.c.o'] + ffmpeg_libs + [
      '-L' + sr / 'lib', '-lstdc++', '-lgcc', '-lgcc_eh', '-lc'])
else
  # ---- the native reference and the sandbox driver --------------------------
  # musl's libm, shadowing glibc's: the guest computes with musl's math, and the
  # reference must compute with the SAME math or the float decode paths diverge.
  # (That was a real gate failure, not a precaution - see docs/PLAN.md.)
  message('building musl\'s libm for the reference if it is not already built...')
  run_command(files('waterbox/build-muslmath.sh'), check: true)
  muslmath = meson.current_source_dir() / 'waterbox/obj-native/libmuslmath.a'

  threads = dependency('threads')
  dl = meson.get_compiler('c').find_library('dl', required: false)
  rt = meson.get_compiler('c').find_library('rt', required: false)

  executable('run-native',
    core_sources + driver_sources + [assets[0], git_version] + files('waterbox/run-native.cpp'),
    include_directories: [inc, ffmpeg_inc],
    dependencies: [assets_table, threads, dl, rt],
    link_with: [chdr_deps_lib, chdr_lib, lzmasdk_lib],
    link_args: ffmpeg_libs + [muslmath])

  mbhost = mb / 'build/meson-linux/source/host'
  executable('run-wbx', files('waterbox/run-wbx.c'),
    c_args: ['-I' + mb / 'source/host'],
    link_args: [mbhost / 'libminiboxhost.so', '-Wl,-rpath,' + mbhost])
endif
