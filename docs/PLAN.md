# PPSSPP -> Chimera waterbox core: the plan

Written 2026-08-24 at the start of the effort; update as milestones land. The goal is a
working, deterministic, waterboxed PPSSPP core package. Software renderer only, IR
interpreter first, and no unnecessary subsystems (no retro achievements, no networking
backends, no UI, no shader translation).

## What the previous attempts teach

Three prior bodies of work exist (see README for links):

1. **Native BizHawk port** (worked, with desyncs). Desyncs came from PPSSPP's own
   savestate (`DoState`) machinery not capturing everything - threads, HLE internals,
   timing. Lesson: do not use PPSSPP savestates at all. miniBox snapshots the whole
   guest machine, so every byte of mutable emulator state must simply live in guest
   memory; then savestates are exact by construction.
2. **Waterbox attempt** (BizHawk's old waterbox, branch `ppsspp_wbx` + TASEmulators/ppsspp
   branch `wbx`): built PPSSPP's libretro flavor inside the box; required a ~90-file,
   +-4.8k-line patch series, the bulk of it **dethreading** (BizHawk's waterbox has no
   guest threads), plus: omitting sleeps, memfile for I/O, removing mkdir, removing
   GPU/gl deps, removing rcheevos. Died on an unsolved crash. Lessons:
   - miniBox HAS deterministic green threads (futex-backed pthreads, cooperative,
     one-at-a-time). Keep PPSSPP's threads; drop the dethreading patches entirely.
     This removes the most invasive and crash-prone part of the old attempt.
   - The old patch series (fetchable: TASEmulators/ppsspp branch `wbx`, series above
     merge `ecbbadd604`) is the reference list of host-dependency landmines.
3. **headlessPPSSPP**: a curated ~700-file source list that links outside CMake. Confirms
   the "own build, no CMake" approach and is a starting superset for our list.

## Architecture decisions

- **Upstream pin**: `extern/ppsspp` = hrydgard/ppsspp @ v1.20.4 (2026-05-16), unmodified.
  Local changes live in `patches/` (git apply at build time), kept as small as possible;
  prefer solving problems in the adapter. If the series grows, move to a proper fork
  under ToolAssisted-run.
- **Driver, not libretro**: we embed PPSSPP the way `headless/Headless.cpp` does
  (PSP_InitStart/PSP_InitUpdate + run loop until `CORE_NEXTFRAME`, the libretro
  frame-stepping model), with our own minimal System_* stubs. The libretro layer adds
  UI/GL surface area we do not want. With `--graphics=software` the upstream headless
  host needs NO graphics context at all (`HeadlessHost` base class, null DrawContext).
- **CPU**: `CPUCore::IR_INTERPRETER` to start (deterministic, no codegen). The x86-64
  JIT runs on guest-native x86-64 and miniBox allows RWX pages, so enabling it later is
  an option, but only after the interpreter gate is green.
- **GPU**: `GPUCORE_SOFTWARE`. Note softgpu itself emits x86 at runtime (DrawPixelX86,
  SamplerX86, RasterizerRegCache); if that misbehaves in the box there are C fallbacks.
- **One curated source list** (`waterbox/sources.list`), compiled twice: natively
  (reference + debugging) and for the guest (musl toolchain, `-mcmodel=large -fno-pic`).
  Same sources, same defines; the native flavor is the equivalence-gate reference,
  exactly the QuickerNesHawk pattern.

## The machine mapping (miniBox spec -> PPSSPP)

- **ISO/ROM**: mounted host-side via `wbx_mount_file` (read-only, hash-bound to
  savestates), read lazily by the guest through VFS open/lseek/read. NEVER slurped into
  guest memory (UMDs are up to ~1.8 GiB). PPSSPP side: a custom `FileLoader` doing
  lseek+read (no pread in the miniBox syscall surface).
- **Assets/flash0** (PPGe atlas, font files): packed into one mounted blob; a custom
  `VFSBackend` (PPSSPP `g_VFS`) serves it. `flash0:` goes through `VFSFileSystem`,
  which reads via g_VFS, so one backend covers both. No opendir/getdents in the box, so
  upstream `DirectoryReader` cannot be used.
- **Memory stick** (savedata, PSP/SYSTEM): must be guest-memory-resident (writable
  mounted files block savestates). Implement an in-memory `IFileSystem`
  (std::map-backed tree) registered in place of `DirectoryFileSystem`. It is savestated
  automatically because it lives in guest RAM. Persistent-data ABI ships it out as the
  core's persistent payload (bundle: "memstick").
- **Time**: the box clock is a CONSTANT (`clock_gettime` = 1495889068.0s always);
  `nanosleep`/`clock_nanosleep` are yields. Anything in PPSSPP that waits for real time
  to advance will spin/hang. CoreTiming is virtual (safe). Patch `time_now_d()` and
  friends to derive from CoreTiming cycles so "real time" advances deterministically
  with emulation - this also kills a whole class of desyncs. (The old attempt's
  "Omitting sleep" commit is the crude version of this.)
- **Threads**: keep. miniBox green threads are deterministic; the danger is a busy-wait
  that never issues a syscall (never yields -> livelock). Audit spin loops
  (`std::atomic` polling); PPSSPP mostly blocks on condvars (futex - fine).
  ThreadManager worker count: fix at a small constant (never `cpu_info.num_cores` -
  thread count and scheduling order must not depend on the host machine).
- **No exceptions to determinism**: no getrandom (trap), sockets trap at runtime (net
  HLE modules still compile and register; a game that calls them gets failures, which
  is fine and matches "no network" semantics).
- **Memory layout**: PSP RAM is 32MiB + 2MiB VRAM + ~4MiB scratch, but PPSSPP's own
  heap (softgpu bins, IR blocks, kernel objects, STL) is the big consumer. Start
  generous (sbrk 512MiB, mmap 1024MiB, plain 64MiB, sealed 32MiB, invis 64MiB), then
  measure and shrink (savestate cost scales with the declared layout).

## Frame loop (adapter)

- Input: HLE-level injection like libretro (`__CtrlUpdateButtons`, `__CtrlSetAnalogXY`).
  Lag detection: instrument sceCtrl's read/peek entry points.
- Video: after `CORE_NEXTFRAME`, read the current display framebuffer (sceDisplay's
  fb pointer/stride/format) out of VRAM, convert to BGRA 480x272.
- Audio: pull a fixed 44100Hz stereo block per frame from `__AudioMix`
  (host-side pull does not feed back into emulation).
- Vsync: 60000000/1001001 (the PSP's exact 59.9400...Hz), constant.

## Milestones

- [x] M0 survey: miniBox spec, prior attempts, upstream headless path. (2026-08-24)
- [x] M1 skeleton: repo layout, submodule pinned, plan committed. (2026-08-24)
- [x] M2a native compile: curated source list compiles natively with our defines.
      (2026-08-24)
- [x] M2b native run: 275/313 pspautotests pass (.expected compare); remaining:
      psmfplayer (no ffmpeg), screenshot-compare gpu tests, atrac second-buffer,
      savedata, io/cwd. Homebrew EBOOT still to test. (2026-08-24)
- [x] M3 guest link + run: core.wbx boots and runs deterministically in the
      sandbox. Root causes fixed on the way: musl 128KB thread stacks, O_CLOEXEC
      fcntl, prctl thread names, guest sigaltstack/sigaction, exec-memory mmap
      hint, one-open-per-file VFS (BlobFileSystem boot), fs-relative
      thread_local stomping HOST TLS, and (in miniBox) the missing host
      sigaltstack that made stack-page faults undeliverable - very likely the
      year-ago attempt's unsolved crash class. (2026-08-24)
- [x] M4 gate: run-gate.sh green - 5/5 free tests, 120 frames each,
      native == sandbox == per-frame-savestate-rerecord, all digests
      (video, audio, RAM, VRAM, scratchpad) bit-identical. ~60fps in-sandbox
      on light content with the IR interpreter. (2026-08-24)
- [x] M5 package: waterbox.config (PSP Controller, 480x272, 60000000/1001001,
      empty settings surface for now), default_keybinds.json, deterministic
      build-package.sh -> <chimera>/build/Cores/ppsspp.zip, and
      tests/run-frontend.sh GREEN: Chimera loads the package, runs a .prx 120
      frames with a RAM slice byte-identical to the native reference, and
      adopts the package's 12 keybinds. Note: the frontend mounts the file as
      the literal name "rom", so the fake DiscID differs from a named boot -
      the gate boots its native reference from a file named "rom" to match.
      (2026-08-24)
- [~] M6 in progress:
      - [x] RAM memory stick (ram-filesystem.cpp): sceIo's ms0: is an in-guest
            case-insensitive FAT-ish tree; savedata makedata/autosave/filelist
            pass and GATE green at 900 frames (savedata written in guest
            memory, captured by machine savestates). (2026-08-24)
      - [x] persistent-data ABI: GetPersistent*/PutPersistent export/import
            the memstick tree ("ChimMS01" flat stream, sorted, deterministic;
            id "memstick", label "Memory Stick"). The card store is
            process-global like a physical stick (survives PSP_Shutdown).
            Round-trips byte-identically through the sandbox. (2026-08-24)
      - [ ] real game content (.iso/.pbp) - none on this machine; needs the
            user's files. ISO path (ISOFileSystem over one FileLoader) is
            architecturally exercised but untested with a real image.
      - [x] analog input, end to end: the frontend's existing axis contract
            (config "axes" + guest SetAxis export, AnalogBind adoption from
            default_keybinds.json) is wired to the core - "P1 L-Stick X/Y",
            -128..127, positive Y = up, bound to the gamepad's left thumb by
            default. run-gate drives the sandbox through SetAxis against the
            native packed-analog path with a wandering stick and requires
            identical digests (6/6 incl. ctrl.prx); injection into sceCtrl
            verified directly (values reach ctrlCurrent and, with analog
            sampling mode enabled, the sampled buffers). The frontend gate
            confirms Chimera loads the axes declaration (the engine aborts
            if SetAxis were missing) and adopts the 2 analog binds.
            (2026-08-25)
      - [x] settings surface: pspModel (PSP-1000 32MB / Slim 64MB), language,
            nickname, buttonPreference - all sync, read from the mounted
            settings channel via the miniBox jsmn helper. run-wbx grows
            --settings, run-native --psp-model; a psp-1000 boot is
            bit-identical native-vs-sandbox, and the frontend gate proves the
            sync-settings dialog path reshapes the machine (the RAM domain
            reports 32MB). (2026-08-25)
      - [x] the x86 JIT as a sync setting ("cpuCore": ir-interpreter default,
            interpreter, jit). Generated code is ordinary guest memory at
            fixed deterministic addresses, so the guest is fully
            deterministic under JIT (plain == rerecord == rewind == repeat on
            ALL digests, RAM included). Cross-build RAM equality is impossible
            BY CONSTRUCTION under JIT: block linking writes cache-offset
            emuhack opcodes into PSP RAM and generated-code sizes differ
            between the glibc and musl builds - so the gate's JIT leg compares
            native-vs-guest minus RAM plus full-digest guest rerecord. The
            two CPU cores are different machines (digests differ), hence the
            sync setting. On Beta Bloc (softgpu-bound) JIT ~= interpreter at
            ~180fps; the win is CPU-heavy 3D games. (2026-08-25)
      - [x] ffmpeg: PPSSPP's pinned fork built from source for BOTH flavors
            (build-ffmpeg.sh: decode-only, --disable-asm,
            --disable-runtime-cpudetect, --disable-pthreads), USE_FFMPEG on.
            Beta Bloc's intro video DECODES (was black), and the movie stays
            frame-identical in sync - the full game gate is green with video
            and audio active. One real divergence surfaced and was fixed
            properly: at3_standalone (and any float decoder) builds its DSP
            tables with libm, and glibc's and musl's transcendentals differ
            in last-ulp cases; the native reference now links musl's libm
            (build-muslmath.sh, shadowing glibc's math symbols), so both
            builds compute with the SAME math. sqrt is IEEE-exact everywhere;
            sin/cos/pow/exp2 are not - this shim closes that class of
            cross-build divergence for good. (2026-08-25)

Decision, 2026-08-25: the frontend keeps mounting the rom under the fixed name
"rom" and does NOT pass the original filename into the guest. A filename-
derived machine (PPSSPP's fake DiscIDs for homebrew) would desync a movie the
moment the user renames the file; the constant name gives a constant identity,
and real games take their DiscID from PARAM.SFO regardless. run-wbx's
/<basename> + rom.name mount is a testing convenience only.

Note on pspautotests counts: booting a bare .prx now serves umd0: from the
boot file ALONE (BlobFileSystem), matching the frontend's single-mounted-file
reality; the ~20 tests that read sibling data files (audio/atrac second half,
mp3, font/*) no longer apply to this build's native sweep. Real games are
single-file (.iso/.cso/.pbp) and unaffected. The correctness metrics are the
gates, which are green.

## Test content (no copyrighted ROMs)

- pspautotests (upstream submodule; the `-g` "tests_good" set, ~314 tests, runs under
  the software renderer on CI upstream).
- Homebrew EBOOT.PBPs for full-game frames (e.g. free homebrew demos), for the gate's
  video/audio digests.

## Known open questions

- Does `PSP_InitStart`'s boot pipeline touch host paths we must fake (config dir,
  memstick dir) before our filesystems are registered? (Headless forces
  `g_Config.memStickDirectory`; we redirect at IFileSystem level.)
- ffmpeg: NOT used (at3_standalone decodes Atrac3+; video (Mpeg/H.264 in games' PMFs)
  needs ffmpeg upstream - out of scope initially; sceMpeg without ffmpeg stubs out,
  some games' cutscenes will be black/skipped. Revisit later (upstream's own
  ffmpeg fork builds statically; it is big but self-contained).
- zstd/snappy/zlib: zlib is required (many paths); zstd required by Serializer &
  ReplacedTexture includes even if we never save PPSSPP states - keep, it is small.
- The old unsolved crash: unknown root cause, was in the dethreaded libretro build on
  BizHawk's waterbox. Not carried forward as a known issue; the architecture that
  produced it (dethreading + libretro + old waterbox) is gone.
