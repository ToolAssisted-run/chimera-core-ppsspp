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

- **Upstream pin**: `extern/ppsspp` = hrydgard/ppsspp @ master 5fa79c6871 (2026-10-15), unmodified
  (deca5f432d from 2026-10-13, 761cf347e6 from 2026-10-09, v1.20.4 before that; see "The pin
  moved to master", "The pin moved again" and "The pin moved for Resistance: Retribution").
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
  automatically because it lives in guest RAM. The savedata export group
  (GetSaveDataFileCount/Name/Size/Buffer, chimera docs/save-data.md) is the user's way
  out: Emulator > Export Save Data enumerates the tree as (path, bytes).
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
- **A frame is one vblank** (2026-09-19, chimera#58, patch 0002). It used to be
  "run until `CORE_NEXTFRAME`", which is PPSSPP's own notion - the game
  PRESENTED - and a game may present twice a vblank (`sceDisplaySetFramebuf`
  in IMMEDIATE mode flips at once) or once in several: Mortal Kombat
  Unchained made 107 movie frames a second, so its 27838-frame movie was
  4:20 long and the site's frames-to-time said 7:43. PPSSPP's run loops stop
  on a `coreState` change and on nothing else (the tick target handed to
  `Core_RunLoopUntil` is never compared), so the vblank itself has to set
  the state: `DisplayFireVblankStart` calls the driver's
  `chimera_vblank_start`, which makes the run loop return, and a flip that
  lands inside the frame is a host-frame boundary for the GPU (the
  End/BeginHostFrame pair PPSSPP's loop would do there) and nothing more.
  Measured on Crash Tag Team Racing and Ridge Racer: 735.716 audio sample
  pairs per frame, exactly 44100/59.94, no frame longer. This renumbers every
  PSP movie made before it (user-decided, 2026-09-19).
- Video: after the frame, read the current display framebuffer (sceDisplay's
  fb pointer/stride/format) out of VRAM, convert to BGRA 480x272.
- Audio: pull a fixed 44100Hz stereo block per frame from `__AudioMix`
  (host-side pull does not feed back into emulation).
- Vsync: 60000/1001 (60/1.001, the rate PPSSPP models), constant.

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
- [x] M5 package: waterbox.config (PSP Controller, 480x272, 60000/1001,
      empty settings surface for now), default_keybinds.json, deterministic
      build-package.sh -> <chimera>/build/Cores/ppsspp.chimeraCore, and
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
      - [x] savedata export group (replaced the removed persistent-data ABI,
            2026-08-25): GetSaveDataFileCount/Name/Size/Buffer walk the
            memstick tree, relative original-case paths, deterministic map
            order. The card store is process-global like a physical stick
            (survives PSP_Shutdown). Gate legs prove native, sandbox,
            rerecord and engine (chimera-run --export-savedata) produce
            byte-identical trees (makedata.prx, pinned frame 20).
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

Added 2026-08-25: the full sync-settings surface, 17 entries. Machine: cpuCore,
pspModel, cpuClock (locked MHz, 0 = game-controlled), ioTiming (fast /
simulated / umd-slow; upstream's host-clock mode is nondeterministic and not
offered), firmwareVersion, funcReplacements, encryptSave. System parameters
games read: language, nickname, buttonPreference, timeZone, daylightSavings,
macAddress, dateFormat, timeFormat, parentalLevel. Clock: rtcBase. One shared
applier (pspdrv_apply_setting) serves the guest adapter and run-native's
generic --set key=value, so the two sides parse identically; a six-setting
combined run changes the machine and stays native == sandbox bit-identical.

Added 2026-08-25: the "rtcBase" sync setting - the console's clock at
power-on (YYYY-MM-DD HH:MM:SS, default the sandbox epoch), parsed with plain
civil-date arithmetic and fed to the pinned RTC base. A different clock is a
different machine (rtc.prx digests change), native == sandbox == rerecord
under any clock, and movies record it like every sync setting.

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

Fix, 2026-08-25: SYSPROP_SKIP_UI now answers false. The headless driver we
started from says true, which makes __PPGeInit skip loading ppge_atlas.zim -
so the embedded atlas was never read and every sceUtility dialog (savedata
prompts included) would have rendered without text or graphics. With the flag
off the atlas decodes from the embedded assets into a 2MB kernel-RAM texture
at boot, which reshapes the machine; all gates re-proven green after the flip
(core 6/6+JIT, Beta Bloc movie native==sandbox==rerecord with the run reaching
the same on-screen states, frontend 3/3). Embedded-asset status, verified:
vfpu LUTs load (gates would diverge without them), flash0 fonts load (the
system-font autotests - fontlist, fontinfobyindex, resolution, find, optimum -
pass and gate native==sandbox), atlas loads (savedata autotests pass with a
nonzero atlas pointer). The font tests that fail (fontinfo and friends) fail
only because they open a sibling ltn0.pgf, i.e. the single-file boot note
above, in both builds identically.

Feature, 2026-08-25: real system fonts through the firmware channel. PPSSPP
bundles free replacement fonts in flash0/font, but a real console's fonts
(dumpable from flash0:/font, not distributable) have different glyphs and
metrics - a different machine wherever a game reads text through sceFont.
The package now declares all 19 registry font files (ltn0-15, jpn0, kr0,
zh_gb) as optional firmware: the user points Emulator > Firmware at their
dumps, the frontend mounts each under its file name, the guest adapter reads
them once and hands them to a font overlay VFS backend registered ahead of
the embedded assets, so sceFont's flash0:/font reads find the real bytes
file by file. Revised 2026-08-25 (user decision: the firmware channel is an
enumeration of known files): the 18 console fonts pin the exact size a real
console reports (sceFont's registry values, recorded from hardware by
pspautotests) - a file of any other size is refused as the wrong file;
zh_gb.pgf stays unpinned (game-shipped, no single authoritative size, and
the gates' free test content). No public authoritative SHA1 list of the
real fonts exists (PPSSPP's docs list only filenames; no database carries
hashes of Sony's files), so the sha1 lists start empty: a right-sized dump
loads as "unrecognised - used anyway", and the movie header's Firmware line
records id=SHA1 for every provided font, with playback warning on mismatch.
Hashes pinned 2026-08-25 from Sony's final official firmware. Recipe, for
anyone re-deriving them: PPSSPP master (not the v1.20.4 pin of that day) carries
Core/Util/PSARUnpack.cpp, written to pull flash0:/font out of an official
updater; compiled against our native object set - with master's
Core/ELF/PrxDecrypter.cpp, whose PSAR tags v1.20.4 lacks - it decrypts the
6.61 updater EBOOT.PBP (SHA1 bc10ad2114e1c349f23ee2b130d82f7e863460ac,
32580549 bytes) and writes the 21 files under flash0:/font. Two sharp
edges: pass the tool an ABSOLUTE output path (PPSSPP's Path silently fails
to create relative directories, so every write fails with the archive
otherwise fully decoded), and the extracted fonts are copyrighted - they
stay out of every repo, only their hashes go in. Confirmation the
decryption is right: all 18 extracted sizes equal the registry values
already pinned from hardware, and every one differs from the free
replacement (real ltn0 69108 vs free 38236). The firmware also carries
arib.pgf, gb3s1518.bwfon and imagefont.bin, which sceFont never loads, so
they get no declaration; zh_gb.pgf is NOT in the firmware at all (a real
console's Chinese font is the gb3s1518 bitmap), which settles it as
game-shipped and unpinned. A dump from an older firmware revision is used
and recorded, just reported as unrecognised; add its hashes here to label
it ok. Since 2026-08-25 the frontend also uses a font that is not the
declared size at all (chimera 7023db5: a declaration says what a core
expects, not what the user may supply), so a user's own replacement font
loads over any of these, is flagged custom in the Firmware window and in
an on-screen warning at load, and has its hash recorded in the movie -
which is what makes such a machine reproducible. Proven end to end: a
38236-byte file against a 999999-byte declaration reaches sceFont and
changes the machine. Verified with the real fonts: all 18 mounted at once (jpn0 is
1.5MB) run native==sandbox==rerecord, and the Chimera frontend boots the
machine with all 18 provisioned through the Firmware store. The wrong-size refusal is
gate-checked by hand (frontend refuses a 38236-byte ltn0.pgf, machine
unchanged); the provisioning legs stay on zh_gb.
PPGe (savedata dialog rendering) stays on the atlas: its alternative is a
host-side rasterizer over host fonts, which is nondeterministic and does not
read PGF; the dialogs are PPSSPP's own HLE artwork either way.
Proof: run-gate.sh grew a fonts leg (a provided zh_gb.pgf - free bytes, a
copy of bundled ltn0 - grows sceFont's internal list, fontlist.prx digests
change, native==sandbox==rerecord with the override mounted), and
run-frontend.sh grew a firmware leg (the same font provisioned through the
CoreFirmware config reaches the guest; whole-RAM hash changes). Native
parity via run-native --font-dir DIR; run-wbx mounts via --firmware id=path.

A truncating open erases nothing (2026-10-08, chimera#212). LittleBigPlanet
stopped two button presses in with "save data load failed because the data
is corrupt". It installs a 4 MiB archive on the stick a megabyte at a time,
and opens the file "truncating" for every piece. On a PSP that open erases
nothing: the file ends where the handle last wrote, once it is closed, and
seeking past the earlier pieces has them back. PPSSPP's folder-backed stick
does the same (`DirectoryFileHandle::needsTrunc_`); ours cleared the file at
the open, so only the last piece survived and the game read zeros. The RAM
stick now keeps the same rule, per handle. Proof: gate legs `pieces` (the
stick alone; fails on the old code) and `lbp` (the game: the archive has its
first megabyte, native==sandbox==rerecord over 2100 frames; by hand it goes
on through its film and title into the first level).

## Test content (no copyrighted ROMs)

- pspautotests (upstream submodule; the `-g` "tests_good" set, ~314 tests, runs under
  the software renderer on CI upstream).
- Homebrew EBOOT.PBPs for full-game frames (e.g. free homebrew demos), for the gate's
  video/audio digests.

## Known open questions

- Does `PSP_InitStart`'s boot pipeline touch host paths we must fake (config dir,
  memstick dir) before our filesystems are registered? (Headless forces
  `g_Config.memStickDirectory`; we redirect at IFileSystem level.)
- ffmpeg: built from upstream's own fork, statically, for both flavors
  (`waterbox/build-ffmpeg.sh`, run by meson at configure time), so the video in
  games' PMFs plays. It was out of scope at first: at3_standalone decodes
  Atrac3+ without it, and sceMpeg without ffmpeg stubs out.
- zstd/snappy/zlib: zlib is required (many paths); zstd required by Serializer &
  ReplacedTexture includes even if we never save PPSSPP states - keep, it is small.
- The old unsolved crash: unknown root cause, was in the dethreaded libretro build on
  BizHawk's waterbox. Not carried forward as a known issue; the architecture that
  produced it (dethreading + libretro + old waterbox) is gone.

## The pin moved to master (user-decided, 2026-10-09, chimera#228)

Asked for: two fixes to video playback that upstream merged after v1.20.4
(hrydgard/ppsspp#21907 and #22309; Need for Speed: Shift, Obscure: The
Aftermath). Of cherry-picking them, waiting for v1.20.5 and moving the pin to
master, the owner chose the last. The pin is 761cf347e6, master on the day.

Upstream had moved a long way - 887 files in the directories this core
compiles, a new interpreter, a Media Engine with its own scheduling, new
video and audio codec modules - and the port was small, which is what
`sources.sh` describing the set by shape was for:

- `ext/minimp3` is gone upstream and from the list; `Core/EmuThread.cpp` and
  `Core/CmdLine.cpp` are the application's and are left out.
- Patch 0001 lost its three camera hunks (upstream took the platform cameras
  out of Core), follows the memory stick's mount from sceIo.cpp to System.cpp
  and the boot file's from `umd0:` to `host0:`, and gives PortManager.h the
  two miniupnp types its members now name. Patch 0002 is the same hook.
- The driver: the PPGe atlas is asked for through `SYSPROP_IS_HEADLESS`, a
  program's stdout comes through `Core_RegisterDebugOutputListeners`, flash0
  is under `nandRootDirectory`, and four new symbols are stubbed.
- FFmpeg's fork moved with the pin and both flavours' builds of it were made
  again (the old ones are kept beside them as `ffmpeg.stale-2026-10-09`).

**One thing the gate caught.** `adsrcurve.prx` came out with different RAM in
the two flavours from frame 20 on, each flavour agreeing with itself.
`SeparateSASThread` defaults to "the host has more than one core" and hands
the SAS mix to a host thread; with the Media Engine's new scheduling its
result reached the program's memory at a different moment natively. The
driver now keeps the mix on the machine's thread, whatever the host.

**What it does to existing movies.** A project is pinned to the package it
was made with, so nothing made before is replayed on this build unasked. Where
one is moved over: the committed Beta Bloc movie (4188 frames) is at the same
menu at frames 1000, 2500 and 4187 on both builds, but its picture, sound and
RAM digests all differ; The 3rd Birthday, run 5400 frames with Cross pressed
every 90, is in the same scene at each of six frames on both. The machine is
not the same in detail, and a movie that leans on exact timing may not
survive the move.

**Not shown.** Neither game of the report is here, so the fix itself is
unproved on them. Gate: 12 passed, 1 skipped, as on the old pin.

**What the gate did not catch, the same day.** The first build on the new pin
could not start any game opened through a project - which is every game a
person opens. Upstream's new `LoadCompanionElfDebugInfo` lists the folder the
game is in, looking for an ELF with debug symbols. Both gates hand the
program over as a rom, `/name`, whose folder does not open; a project mounts
it in the disc slot under its bare name, that folder opens, and reading it
is a system call (getdents64) the sandbox does not provide: the guest was
killed inside Init and the frontend said "could not load this file". The
reporter found it in an hour (chimera#228). Patch 0001 takes the lookup out
of the sandbox build, and the frontend gate has a leg that writes a project
for the pinned triangle.prx and runs it in the engine (`project:boot`) -
which fails on the build that was published. The other directory listings
in Core were there on the old pin too and are not reached. A gate that never
opens a game the way a user does was the hole, and a new pin is when it
costs.

## The pin moved again (user-decided, 2026-10-13, chimera#228)

Asked for: upstream's newer software renderer (hrydgard/ppsspp#22486 and the
work around it), which the reporter of chimera#228 named on 2026-10-10. The
answer then was "not now"; on 2026-10-13 the owner asked for the move. The
upstream version this core builds is now deca5f432d, master as of 2026-10-11
18:24 (+0200): 139 commits after the last one, 32 of them in `GPU/Software`.

**Why not the newest commit.** Master on the day was 4c48f85664, and the core
was first moved there. Its tests passed, and Crash Tag Team Racing no longer
started: a flat picture, and a log full of "Path is relative, but current
directory not set for thread" and "file does not exist". The cause is
upstream's 4fda53a62e (2026-10-11 19:08, "sceKernelLoadModule: Put the module
in memory when the wait ends"): the module's file is now read later, from a
timed event, where the thread that asked is not the current one, so a module
named by a relative path (`prx/sc_sascore.prx`) is not found. It is an
upstream fault one day old. Of pinning the commit before it, carrying a patch
of our own for it, and taking master as it was, the owner chose the first.
deca5f432d is that commit; the 90 commits after it are mostly Android, menu
and Vulkan work. The core moves on when upstream has fixed it.

What the move needed:

- Upstream removed the software renderer's own code generators (the pixel and
  sampler JITs) and its setting. Patch 0001 lost the two hunks that guarded
  them, the driver no longer sets `bSoftwareRenderingJit`, and the removed
  files left the source list by themselves (`sources.sh` describes the set by
  shape).
- `GPU/Software/Rasterizer.cpp` keeps a line's pixels in a thread-local
  vector. The sandbox build has no thread-local storage through `%fs` to give
  it, so there it is an ordinary static (patch 0001): the sandbox's threads
  run one at a time, and the vector is filled and read within one call.
- Upstream now keeps the JIT's block handles out of the PSP's memory, in a
  table of its own ("block shadow", hrydgard/ppsspp#22494), and on a 64-bit
  host reserves 1 GiB for it. The sandbox's whole memory arena is 1 GiB, and
  nothing started: "arena exhausted ... 1024 MiB wanted". Upstream has a
  256 MiB table for iOS and the Switch; patch 0001 selects it for this core,
  in both flavours, so the two stay the same machine.
- Patch 0003: two hunks of `VulkanLoader.cpp` were ported by hand (upstream
  renamed the "library is linked in" case to `PPSSPP_VK_STATIC`). Patch 0002
  is unchanged.

Measured on Linux (2026-10-13), the package before against the package after,
software renderer, every frame drawn, through `chimera-run`:

| Game | Frames | Before | After |
|------|--------|--------|-------|
| Beta Bloc, its committed movie | 4188 | 8.9 s | 7.5 s |
| Beta Bloc, no input | 3600 | 7.5 s | 6.5 s |
| Ridge Racer (.chd) | 1500 | 7.9 s | 6.8 s |
| Arcade Essentials | 1500 | 3.2 s | 2.8 s |
| Crash Tag Team Racing (.iso) | 1500 | 3.5 s | 3.3 s |
| Crash Tag Team Racing (.chd) | 1500 | 3.9 s | 3.4 s |

So 5 to 15 percent less time, with the picture at the last frame byte for
byte the same before and after in every row (and at frames 1000, 2500 and
4187 of the Beta Bloc movie). One run each, so the small differences are not
to be read closely. The PSP's memory at the end is NOT the same before and
after (Beta Bloc's movie, Crash Tag Team Racing): the machine differs in
detail, as it did at the last move, and a movie that leans on exact timing
may not survive being moved over. A project stays with the package it was
made with.

Tests at deca5f432d: `run-gate.sh` 16 passed, 0 failed, 1 skipped (the
LittleBigPlanet image is not here); `tests/run-frontend.sh` 6 passed,
including the one that opens a program through a project.

On Windows, the same package through `chimera-run.exe`: with the software
renderer, the Beta Bloc movie and 1500 frames of Crash Tag Team Racing end
with the memory and the pictures of the Linux runs, byte for byte. With the
Vulkan renderer on a GeForce GTX 1060 (Vulkan 1.4.312) both games run and
draw their scenes; Beta Bloc's memory at the end is the software renderer's,
Crash Tag Team Racing's is not, as before the move (see "The Vulkan
renderer"). Beta Bloc's movie took 8.6 s in software and 7.7 s on the card.

**Not shown.** No game with heavy 3D was timed for long, and the package
before was not timed on Windows. The reporter's own games were not tried.

## The pin moved for Resistance: Retribution (2026-10-15, chimera#287)

Asked for: three upstream changes for Resistance: Retribution, merged on
2026-10-14 and 2026-10-15 (hrydgard/ppsspp#22571, #22581, #22582: the smoke,
the crashes, the muzzle light). The upstream version this core builds is now
5fa79c6871, master as of 2026-10-15 17:46 (+0200): 390 commits after the
last one. It also holds #22583, upstream's fix for the Crash Tag Team Racing
fault that kept the last move one commit short ("The pin moved again"), so
the three changes were not taken alone: the commit right after them still
had that fault.

What the move needed:

- **The JIT.** Upstream's x86 JIT is now built on its IR, like the ARM ones
  ("every native JIT is IR-based"), and the old one is gone. The CPU Core
  setting's `jit`, the default, is that one now. This core builds PPSSPP
  with masked memory (the sandbox cannot map the PSP's memory twice), a mode
  upstream builds only for 32-bit hosts: on x86-64 the new JIT added the
  64-bit memory base to a register as an immediate, which x86 cannot encode,
  and every game stopped at its first block ("Only MOV can take 64-bit
  imm"). Patch 0004 uses the register that holds the base instead, as the
  unmasked build does (RBX, loaded by the dispatcher and never handed out),
  at the four places: a register made a pointer and back, a load or store's
  address, and the check that an address is valid. With it the JIT and the IR
  interpreter give the same picture, sound and video memory on the test
  program, natively and in the sandbox.
- **The test script and time zones.** The sandbox has no time zone; the
  native reference used this computer's (UTC+2), and LittleBigPlanet stats
  files on its disc, whose dates came out two hours apart. `run-gate.sh` now
  runs everything in UTC. The shipped core was never affected.
- **The build.** The Vulkan bridge's generated header was made once and
  never again: the step that makes it named no inputs, so a newer miniBox
  generator was not rerun and `vk-host.c` did not compile. It now names the
  generator, `vk.xml` and the list of commands.
- The three patches applied unchanged. FFmpeg's submodule did not move.
  Upstream's new `AccurateVdot` (bit-exact VFPU dot products) is on by
  default and the core leaves it so.

Measured on Linux (2026-10-15), the published package (Development build
dd161279) against this one, default settings (JIT, software renderer),
1500 frames from power-on with no input, through `chimera-run`:

| Game | Before | After | Picture at frame 600 |
|------|--------|-------|----------------------|
| The 3rd Birthday | 2.40 s | 2.41 s | the same |
| Beta Bloc | 2.00 s | 1.92 s | the same |
| Crash Tag Team Racing (.iso) | 2.43 s | 2.39 s | the same, a full scene |
| LittleBigPlanet (.cso) | 4.45 s | 4.48 s | the same |
| Ridge Racer (.chd) | 6.32 s | 5.94 s | the same |

One run each. Tests: `run-gate.sh` 17 passed, 0 failed, 0 skipped
(LittleBigPlanet included, its install and 2100 frames native == sandbox ==
a state around every frame); `tests/run-frontend.sh` 6 passed.

**Not shown.** Resistance: Retribution is not here, so the three changes
asked for were not seen to work. Nothing was run on Windows or on a real
graphics card; the Vulkan tests ran on Mesa's software Vulkan. Whether a
movie made with the package before plays back with this one was not tried:
the JIT is another one, and the memory of the last move's games differed.

## The Vulkan renderer (user-decided, 2026-10-11, chimera#248)

The owner decided that every core whose emulator has a Vulkan renderer offers
it. This core had the software renderer only. Now there is a Renderer
setting: `software`, the default, and `vulkan-hw` - PPSSPP's Vulkan renderer,
drawing on the computer's graphics card through Chimera's Vulkan bridge. It
is in the guest only (the reference flavour has no Chimera to hand it a
device), it draws at the PSP's own 480x272, and it has nothing to fall back
on: on a computer that hands no Vulkan over, Init refuses and says why.

The renderer is part of the machine, as the CPU core is. The two renderers do
not account the same time for drawing, and a game that waits on the GPU then
runs differently: Crash Tag Team Racing's memory differs between them by
frame 600. Beta Bloc and the test programs show no difference. What does NOT
depend on anything outside the core is the machine under Vulkan: its memory
at frame 3600 of Crash Tag Team Racing is byte for byte the same drawn by
Mesa's lavapipe on Linux and by a GeForce GTX 1060 on Windows. Only the
picture is the card's.

What it took (patch 0003, 10 files of PPSSPP, and the adapter):

- **The sources.** `GPU/Vulkan`, `Common/GPU/Vulkan`, the shader translator
  and the memory allocator (waterbox/sources.sh, `vulkan`), and two of
  PPSSPP's submodules that were not fetched before: `ext/glslang`, compiled
  in without its optimiser and without HLSL, and `ext/SPIRV-Cross`.
- **The functions.** Upstream opens the Vulkan library. The bridge's guest
  half is linked into the core instead, and `VulkanLoader.cpp` asks it for
  each command by name.
- **No window.** Upstream already has it: `VulkanGraphicsContext`'s offscreen
  mode, which its own headless program uses, draws into images of its own.
  What was left was that the instance insisted on a surface extension and
  the device asked for the swapchain one; neither exists here. The adapter
  closes and opens the draw context's frame once per displayed frame, as the
  headless program does, and reads the picture back inside it.
- **Memory.** Upstream requires coherent memory for its push buffers, "to
  not get memory where we have to manually flush". Through the bridge no
  memory is coherent, so the pools flush what they were given before every
  submit of commands, and ask for cached memory. A pool's block that is
  started again for a new frame starts unflushed - forgetting that drew
  Crash Tag Team Racing's title with the previous frame's vertices, in the
  straight run and not after a load, which is how it was found. The
  allocator's blocks are 16 MiB.
- **A load.** A load is a new Vulkan device. PPSSPP already knows how to
  lose a device and get one back (Android takes its away): the adapter calls
  `DeviceLost`, shuts the context down, starts it again and calls
  `DeviceRestore` (`RebuildVulkan` in psp-driver.cpp). While the old objects
  are let go of, no command crosses the bridge (`vk-shim.cpp`).
- **What is only on the card.** `DeviceLost` throws every framebuffer away,
  and a game that has stopped drawing never makes its picture again. So
  before a state the colour of every framebuffer is copied into a block of
  the core's memory, a device lost because of a load keeps the framebuffers
  themselves, and after the restore each gets a new image with the copy
  drawn into it (`FramebufferManagerCommon::Chimera*`). Depth and stencil
  are not carried; no game tried showed a difference.

The core's sandbox runner (`run-wbx`) hands the Vulkan bridge over when
`CHIMERA_GPU` asks, tells the core before each state and after each load, as
the frontend does, and can write a screenshot. `run-gate.sh` also runs
`tests/run-game-gate.sh` now (written on 2026-10-08 and left uncommitted
until this change).

Measured on Mesa's lavapipe (2026-10-12):

- triangle.prx, 60 frames: memory and sound are the software renderer's; 282
  of 130,560 pixels differ by more than 8 levels of 255, along the
  triangle's edges. A state saved and loaded around every frame is the
  straight run, picture included, with no command refused; with the core
  left untold before each state, the picture is not.
- Beta Bloc played by its committed movie, 4188 frames: memory and sound are
  the software renderer's, and a state saved and loaded around every frame
  changes nothing, picture included.

Measured on a GeForce GTX 1060 (Windows, driver 581.42, Vulkan 1.4.312)
through chimera-run.exe, with no input, from power-on:

- Crash Tag Team Racing, 8400 frames (through its opening film to the title
  screen): 8.4 s with Vulkan, 37.5 s with the software renderer. Two Vulkan
  runs end on the same memory and the same picture.
- Twelve frames drawn after a load, compared with the frames first drawn:
  all twelve exact in Crash Tag Team Racing at frames 3600, 6000 and 8400,
  in Beta Bloc at 3600 and in The 3rd Birthday at 3600 and 7200. With the
  core left untold, between one and three of the twelve are not exact.
- Crash Tag Team Racing, eight times back from frame 8400 to 8150 and
  forward again: the last picture and the memory are the straight run's,
  byte for byte. It took 34.2 s against 25.8 s for the same run without
  going back.

Not measured: a game played with input on the card, any card other than this
one, and the frontend's window (only its command-line runner). Not offered:
a resolution above the PSP's own. The `vk:` checks in the test script run on
lavapipe, which is not a graphics card.
