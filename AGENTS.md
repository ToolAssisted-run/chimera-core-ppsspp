# AGENTS.md - PPSSPP core for Chimera

This repository turns PPSSPP (the PSP emulator) into a core for Chimera, a
frontend for tool-assisted speedruns. Upstream is the `extern/ppsspp`
submodule, changed only by the patches in `patches/`; `waterbox/` holds the
driver, the guest adapter, the source list and the gates, and one
`meson.build` builds both the sandboxed guest and the native reference. The
product is one file, `ppsspp.chimeraCore`, which Chimera loads and runs inside
its sandbox (miniBox) on Linux and on Windows. Detail for every step below is
in `docs/BUILDING.md`.

## Layout

- `extern/ppsspp` - upstream PPSSPP, a submodule. Its `pspautotests` and
  `ffmpeg` submodules are the test content and the FFmpeg the build compiles.
- `patches/` - the patch series against `extern/ppsspp`.
- `meson.build`, `meson_options.txt` - both flavours: a cross configure is the
  guest (`core.wbx`), a native one is the reference (`run-native`, `run-wbx`).
- `waterbox/sources.sh` - which upstream sources are compiled, for both.
- `waterbox/setup-guest.sh` - writes the cross file and configures the guest.
- `waterbox/apply-patches.sh` - applies the patches; meson runs it.
- `waterbox/build-ffmpeg.sh`, `build-muslmath.sh`, `build-assets.py` - FFmpeg
  per flavour, musl's math for the reference, the embedded assets.
- `waterbox/build-package.sh` - builds the guest and writes the package.
- `waterbox/psp-driver.cpp` - the driver, shared by both flavours.
  `waterbox/waterbox.cpp` - the guest ABI, guest only.
  `waterbox/run-native.cpp` - its native twin. `waterbox/run-wbx.c` - the host
  driver that runs `core.wbx` in the sandbox.
- `waterbox/ram-filesystem.cpp`, `memory-assets.cpp`, `stubs/`,
  `guest-stubs/` - the memory stick in RAM, the embedded assets, stand-ins.
- `waterbox/run-gate.sh`, `waterbox/tests/run-frontend.sh` - the core gate
  and the frontend gate.
- `tests/run-game-gate.sh` and the `.sol` movie beside it - a gate over a real
  game image and an input movie. `tests/roms/` is gitignored.
- `waterbox/waterbox.config`, `file_slots.json`, `default_keybinds.json`,
  `package-licenses.json` - what the package declares.
- `docs/PLAN.md` - milestones and decisions.

## Set up the build environment

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends meson ninja-build build-essential python3
# the frontend gate also needs: cmake pkg-config mono-complete xvfb
#   libgl1-mesa-dev libx11-dev libxext-dev libasound2-dev, and the .NET SDK 8.0

# in this repository: the PPSSPP submodule and the parts the core uses
git submodule update --init --depth 1 extern/ppsspp
git -C extern/ppsspp submodule update --init --depth 1 \
  ext/zstd ext/cpu_features ext/rapidjson ext/libchdr ext/lua \
  ext/aemu_postoffice pspautotests ffmpeg

# Chimera and miniBox (~/chimera is the scripts' fallback)
git clone https://github.com/ToolAssisted-run/chimera.git ~/chimera
git -C ~/chimera submodule update --init extern/chimera-common-minibox

mb=~/chimera/extern/chimera-common-minibox
meson setup "$mb/build/meson-cpp" "$mb" -Dguest_cpp=true
meson compile -C "$mb/build/meson-cpp"
```

The miniBox build downloads the GCC source for the guest's libstdc++. The
frontend gate needs Chimera built with all its submodules (`docs/BUILDING.md`).

## Build

```sh
# from the root of this repository
mb=~/chimera/extern/chimera-common-minibox

sh waterbox/setup-guest.sh -m "$mb"              # the guest
ninja -C build/meson-guest core.wbx

meson setup build/meson-native "-Dminibox_dir=$mb"   # the native reference
ninja -C build/meson-native

./waterbox/build-package.sh -m "$mb" -r ~/chimera
```

`build-package.sh` alone is the shortest path to a package: it builds
miniBox's C++ flavour and the guest when they are missing, checks `core.wbx`
and packs it. The gates also need the native reference.

## Install the core into Chimera

`build-package.sh -r <chimera>` writes `<chimera>/build/Cores/ppsspp.chimeraCore`:
the cores folder of a Chimera source checkout, so nothing else is needed. For
a release bundle, copy the file into the `Cores` folder beside `Chimera.exe`
(or the folder chosen in File > Core Manager > Change folder...). Chimera
downloads nothing; File > Core Manager lists the folder, Refresh List rescans.
A hand build stamps `<commit>+local` (usually `-dirty` too): testing only.

## Test before you commit

```sh
./waterbox/run-gate.sh
```

It must exit 0. Every leg over pspautotests must be PASS: that content comes
with the submodule, so a SKIP there means the checkout is incomplete. The gate
compares native, sandbox, a save and load around every frame, and a run with
the picture off. CI runs it on every push.

The frontend gate, which CI also runs (needs Chimera built, the package
installed and `build/meson-native`):

```sh
./waterbox/tests/run-frontend.sh --chimera-root ~/chimera
```

It must end with `0 failed`. If Chimera is not built on this machine, say in
the commit that the frontend gate was not run.

`tests/run-game-gate.sh` runs only where the user's game image is in
`tests/roms/`; otherwise it prints SKIP.

## Rules of this repository

- Never commit inside `extern/ppsspp`. A change to PPSSPP is a patch in
  `patches/`, applied by `waterbox/apply-patches.sh` when meson configures.
- That script skips a patch that does not apply, silently. After changing a
  patch or moving the submodule, make sure the tree really carries the series.
- Both flavours compile the same sources with the same defines. A source is
  added or removed in `waterbox/sources.sh`, never in one flavour only.
- Determinism is the product. The guest must not read host time, host
  randomness or anything else that differs between runs, and a savestate must
  round-trip. The gate checks it; a change that breaks it is a bug.
- The native reference must compute what the guest computes: it links musl's
  math library and an FFmpeg built with the same options. Do not give one
  flavour something the other lacks.
- Run the gate before committing. A new leg needs a negative control: show it
  fails when the thing it checks is broken, and say so in the commit.
- Test content in the gates is free content. Never commit game images, save
  data or font dumps. Never add network access.
- Licences: the integration is MIT; `patches/` and `waterbox/psp-driver.cpp`
  are GPL-2.0-or-later (see `LICENSE`).
- Scripts that are executable stay executable (git mode 100755). New
  documentation prose is plain ASCII.
- Commit messages: `type(scope): a full sentence saying what is now true`, or
  `type: ...` without a scope, with `(chimera#N)` when it fixes an issue
  (issues are filed in the chimera repository). Types in use: `feat`, `fix`,
  `test`, `docs`, `ci`. The body says what was wrong, what the gate proves now
  and how the new check was seen to fail.
- A decision or a finding worth keeping goes into `docs/PLAN.md`, dated.
- Do not edit `.github/workflows` unless the task is the workflow.

## Where to read more

- `docs/BUILDING.md` - every build step, option and known failure.
- `docs/PLAN.md` - the reasoning behind the design, milestone by milestone.
- `.github/workflows/chimera.yml` - the authoritative recipe.
- In the Chimera checkout: `docs/porting-a-core.md`, `docs/gates.md` (how a
  gate goes green on a broken thing) and `docs/core-manager.md`.
