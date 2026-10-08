# Building the PPSSPP core

This repository builds one file, `ppsspp.chimeraCore`: PPSSPP (the PSP
emulator) compiled as a sandboxed guest (`core.wbx`) together with the
declarations Chimera reads. One `meson.build` describes both flavours: a
cross configure is the guest, a native configure is the reference the gates
compare it against. The steps below are the ones
`.github/workflows/chimera.yml` runs from a fresh clone on a public Ubuntu
runner, in its two jobs (`core-gate` and `frontend-gate`).

Placeholders used below:

- `<core>` - the checkout of this repository. Commands run from it unless
  stated otherwise.
- `<chimera>` - a checkout of https://github.com/ToolAssisted-run/chimera.
- `<miniBox>` - `<chimera>/extern/chimera-common-minibox`, the sandbox host and
  the guest toolchain.

## Requirements

Operating system: CI uses the `ubuntu-latest` runner. Cores are built on Linux
only. The package that comes out runs on Linux and on Windows.

System packages for the core and the core gate, as the `core-gate` job
installs them:

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends meson ninja-build build-essential python3
```

The `frontend-gate` job, which also builds Chimera, installs:

```sh
sudo apt-get install -y --no-install-recommends \
  meson ninja-build build-essential cmake pkg-config python3 \
  mono-complete xvfb \
  libgl1-mesa-dev libx11-dev libxext-dev libasound2-dev
```

The workflow pins no compiler version: the default `gcc` and `g++` are used.

.NET: the `frontend-gate` job uses `actions/setup-dotnet@v4` with
`dotnet-version: '8.0'`. By hand, install the .NET SDK 8.0. Chimera's README
gives the command it expects:
`curl -sSL https://dot.net/v1/dotnet-install.sh | bash -s -- --channel 8.0`.
.NET, Mono and Xvfb are needed only for the frontend gate.

Built from source by the build itself, never taken from the system:

- FFmpeg, from PPSSPP's own pinned fork (the `ffmpeg` submodule inside
  `extern/ppsspp`), once per flavour, by `waterbox/build-ffmpeg.sh`. Decoders
  only, no assembly, no runtime CPU detection, no threads, no network.
- musl's math library, for the native reference, by
  `waterbox/build-muslmath.sh`. The guest computes with musl's math and the
  reference must compute with the same.
- PPSSPP's assets (fonts, tables, atlas), packed into the binary by
  `waterbox/build-assets.py`.

meson runs the first two at configure time: FFmpeg for the flavour being
configured, the math library for the native one.

Downloaded by the build: when miniBox builds its C++ guest toolchain it fetches
the GCC source that matches the host compiler (about 84 MB, with `curl`) to
build libstdc++ for the guest. `curl` is not in this workflow's package lists;
make sure it is installed. Nothing else is downloaded.

Time: the workflow gives `core-gate` 90 minutes and `frontend-gate` 120.

Sources: Chimera is taken at its `main` branch (`CHIMERA_REF` in the
workflow). PPSSPP is whatever commit the `extern/ppsspp` submodule points at.

## Get the sources

Clone this repository, then the PPSSPP submodule and only the parts of it the
core uses. The workflow does it with `actions/checkout@v6` and these commands:

```sh
git clone https://github.com/ToolAssisted-run/chimera-core-ppsspp.git <core>
cd <core>
git submodule update --init --depth 1 extern/ppsspp
cd extern/ppsspp
git submodule update --init --depth 1 \
  ext/zstd ext/cpu_features ext/rapidjson ext/libchdr ext/lua \
  ext/aemu_postoffice pspautotests ffmpeg
```

`pspautotests` is the test content of both gates. `ffmpeg` is the fork the
build compiles.

Get Chimera. For the core and the core gate, miniBox is the only submodule
needed, and that is what the `core-gate` job initialises:

```sh
git clone https://github.com/ToolAssisted-run/chimera.git <chimera>
cd <chimera>
git submodule update --init extern/chimera-common-minibox
```

For the frontend gate Chimera itself is built, so the `frontend-gate` job
checks it out with all submodules (`submodules: recursive`):

```sh
git clone --recurse-submodules https://github.com/ToolAssisted-run/chimera.git <chimera>
```

CI checks out Chimera's `main` branch into `chimera-checkout` inside the core
checkout and passes that path to the scripts.

Where the build looks when you pass nothing:

- `waterbox/build-package.sh` looks for a `chimera` directory beside this
  repository, then `$HOME/chimera`. `-r <chimera>` names it. miniBox is
  `<chimera>/extern/chimera-common-minibox` unless `-m <miniBox>` or the
  `MINIBOX_DIR` variable says otherwise.
- `waterbox/setup-guest.sh` takes `-m <miniBox>` or `MINIBOX_DIR`, and falls
  back to `$HOME/chimera/extern/chimera-common-minibox`.
- The native configure takes `-Dminibox_dir=<miniBox>`. Without it,
  `meson.build` uses `../chimera/extern/chimera-common-minibox` beside this
  repository, and stops with an error when that is not there.

## Build miniBox

One build directory, the C++ flavour. It carries the guest toolchain (musl and
libstdc++ in a guest sysroot) and the host library the sandbox driver links.

```sh
mb=<chimera>/extern/chimera-common-minibox
meson setup "$mb/build/meson-cpp" "$mb" -Dguest_cpp=true
meson compile -C "$mb/build/meson-cpp"
```

CI keeps that directory between runs with `actions/cache@v4` and skips
`meson setup` when its `build.ninja` is already there.

## Build the core

### Patches

`patches/` holds the patch series against `extern/ppsspp` (two patches).
`waterbox/apply-patches.sh` applies it, and meson runs that script every time
either flavour is configured, so there is nothing to do by hand. The script
takes each patch in order: a patch that applies cleanly is applied, and any
other patch is skipped without a message. That makes it harmless to configure
twice. It also means a patch that no longer applies is skipped the same way
as one that is already there.

### The guest

```sh
sh waterbox/setup-guest.sh -m "$mb"
ninja -C build/meson-guest core.wbx
```

`setup-guest.sh` writes the meson cross file `build/guest-cross.ini` (it holds
machine-local paths; `build/` is ignored) and configures `build/meson-guest`
with it, or reconfigures it when it exists. Arguments after `-m` are passed to
meson. Configuring builds FFmpeg for the guest into `waterbox/obj-guest/ffmpeg`
when it is not already built. The result is `build/meson-guest/core.wbx`.

Which upstream sources are compiled is decided in one place,
`waterbox/sources.sh`, for both flavours.

### The native reference

The same sources and the same driver, built for the host: `run-native`. The
gates compare the sandboxed core against it. The same build directory also
holds `run-wbx`, the host driver that runs `core.wbx` in the sandbox. A package
contains neither.

```sh
meson setup build/meson-native "-Dminibox_dir=$mb"
ninja -C build/meson-native
```

Configuring builds FFmpeg for the host into `waterbox/obj-native/ffmpeg` and
musl's math library into `waterbox/obj-native/libmuslmath.a` when they are not
already built. It needs miniBox built first: `run-wbx` links
`libminiboxhost.so`, and the math library is compiled against the guest
sysroot's headers.

The `core-gate` job builds the guest first and the native reference second.

## Build the package

```
waterbox/build-package.sh [-m <miniBox dir>] [-r <chimera root>] [-o <build dir>]
```

The script:

1. configures `<miniBox>/build/meson-cpp` with `-Dguest_cpp=true` when it is
   not configured, and runs `ninja` in it;
2. runs `setup-guest.sh -m <miniBox>` when `build/meson-guest/build.ninja` is
   missing, then `ninja -C build/meson-guest core.wbx`;
3. runs miniBox's `check-wbx.sh` on `build/meson-guest/core.wbx`;
4. stages `core.wbx`, `waterbox.config`, `default_keybinds.json`,
   `file_slots.json`, the licence texts named by
   `waterbox/package-licenses.json` and a `build.json` that records what built
   the package, in `<build dir>/package-staging`. `-o` moves that staging
   directory (default `<core>/build`); it does not move the package;
5. zips them with sorted entries and fixed timestamps into
   `<chimera>/build/Cores/ppsspp.chimeraCore`, packs a second time and stops
   if the two SHA-1 values differ;
6. removes `<chimera>/build/CoreCache/ppsspp-*`, so the next load unpacks the
   new build.

Version stamp, written into the packaged `waterbox.config`:

- CI sets `CORE_VERSION` to the commit, in the `frontend-gate` job:
  `CORE_VERSION=<commit> ./waterbox/build-package.sh -r <chimera>`.
- Without `CORE_VERSION` the script stamps `<commit>+local`, or
  `<commit>-dirty+local` when the tree has changes. The build patches
  `extern/ppsspp` in place, which counts as a change, so a hand build normally
  says `-dirty`.
- `versionDate` is the commit's date in UTC, never the build's.

A hand-built package is for testing. Chimera's publishing script refuses a
version that carries `+local` or `-dirty`.

## Install it into Chimera

Chimera ships no cores and downloads nothing: it has no network code. A core
gets into Chimera because somebody puts the file in its `Cores` folder.

- In a Chimera source checkout the cores folder is `<chimera>/build/Cores/`,
  and `build-package.sh -r <chimera>` has already written the package there.
- In a release bundle, copy `ppsspp.chimeraCore` into the `Cores` folder
  beside `Chimera.exe`, or into the folder chosen in File > Core Manager >
  Change folder... The same file works on Linux and on Windows.
- File > Core Manager lists what is in the folder. Refresh List rescans it.

Published builds are on this repository's Releases page: a rolling `dev`
release on every green push to `main`, and a dated `nightly-YYYY-MM-DD` release
from the scheduled run when `main` moved since the last one. Their asset is
named `ppsspp-<version>.chimeraCore`.

## Run the gates

The gates run on free content: the pspautotests programs that come with the
PPSSPP submodule. CI runs both with nothing provisioned.

### The core gate

```sh
./waterbox/run-gate.sh
```

```
run-gate.sh [-n <native build dir>] [-g <guest build dir>] [-f frames] [file...]
```

Needs `build/meson-native/run-native`, `build/meson-native/run-wbx` and
`build/meson-guest/core.wbx`. The frame count defaults to 120. It exits
non-zero when a leg failed, and also when not one equivalence leg ran.

With no files it runs six pspautotests programs. For each one, the native
build and the sandbox must print identical video, audio and memory digests;
the sandbox again with a save and load around every frame must print the same;
and a run with the picture switched off for its first half must leave the
machine, the sound and the picture drawn afterwards untouched. These legs pin
the IR interpreter on both sides. Then:

- `jit` - under the x86 JIT, native and sandbox agree on everything except
  RAM (which cannot be equal across the two builds by construction), and the
  sandbox is deterministic on every digest.
- `fonts` - a font mounted through the firmware channel reaches the machine,
  native == sandbox == rerecord. The font is a copy of a bundled free one.
- `seed` - save data and DLC zips reach the memory stick before the machine
  starts, the same in both flavours; a zip that reaches outside the stick and
  a file that is no zip are refused.
- `savedata` - what the machine saved leaves through the save data export as
  the same tree natively, in the sandbox and with rerecord.
- `pieces` - a file written through truncating opens keeps every piece (a
  truncating open erases nothing on a PSP until the handle is closed). No
  program runs: `run-native --stick-truncate-test` drives the stick itself.
- `lbp` - with `PPSSPP_LBP` naming a LittleBigPlanet image (UCUS98744), the
  game installs its archive on the stick whole and plays on, native ==
  sandbox == rerecord over 2100 frames. Without the image it says `SKIP`.

A default test program that is missing is a FAIL, not a SKIP: the pinned test
set has moved. A file you name yourself that is missing is a SKIP.

### The real-game gate

```
tests/run-game-gate.sh [game.iso movie.sol]
```

Replays a committed input movie over a game image through both flavours, then
again with a save and load around every frame. Game images are not
distributable: `tests/roms/` is gitignored, and without the image the script
prints `SKIP: no game image at ...` and exits 0. A runner has no image, so
nothing of it runs in CI. The script names the image and the movie it uses by
default.

### The frontend gate

Runs the package inside Chimera, headless under Mono on a private Xvfb
display. Chimera must be built first. From `<chimera>`:

```sh
meson setup build/meson-linux --prefix "$PWD/build" --libdir dll
meson compile -C build/meson-linux
meson install -C build/meson-linux
dotnet build source/gui/Chimera.sln -c Release /nodeReuse:false -p:UseSharedCompilation=false
```

Then, from `<core>`, with the package installed and the native reference
built:

```sh
./waterbox/tests/run-frontend.sh --chimera-root <chimera>
```

```
run-frontend.sh [--chimera-root <path>] [--frames N] [file...]
```

It needs `<chimera>/build/Chimera.exe`,
`<chimera>/build/Cores/ppsspp.chimeraCore` and
`build/meson-native/run-native`. The frame count defaults to 120 and the
default program is pspautotests' `triangle.prx`. It ends with
`N ok, M failed, K skipped` and exits non-zero when a leg failed. Legs:

- `<name>:frontend` - a 64 KB slice of RAM after the run equals the native
  reference's, byte for byte.
- `<name>:settings:model` - a machine-shaping setting reaches the guest:
  `pspModel=psp-1000` boots a 32 MB machine.
- `<name>:keybinds` - the package's default bindings became the frontend's.
- `fontlist:firmware` - a font given through the frontend's firmware store
  reaches the guest.
- `savedata:engine` - the save data exported through the engine
  (`<chimera>/build/meson-linux/chimera-run`) is the same tree the sandbox
  driver exports. SKIP when `chimera-run` is not built; FAIL when
  `build/meson-guest/core.wbx` is missing.

## Files the core needs at run time

None of these is in the repository or in the package. The user provides them.

Firmware, declared in `waterbox/waterbox.config`: none is needed by default.
The core ships free replacement fonts. When the `fontSource` setting is
`sony`, each declared system font becomes a requirement: `ltn0.pgf` to
`ltn15.pgf`, `jpn0.pgf`, `kr0.pgf` and `zh_gb.pgf`. The user dumps them from
their own console's `flash0:/font`.

Project files, declared in `waterbox/file_slots.json`:

- UMD disc (exactly one): the game as `.iso` or `.cso`, or homebrew as `.pbp`,
  `.prx` or `.elf`.
- Save data (optional): one `.zip`, either what Emulator > Export Save Data...
  writes or a zip of save folders taken off a memory stick.
- DLC (any number): `.zip` files of a game's DLC folder as a memory stick
  holds it.

## Troubleshooting

- `ffmpeg submodule missing`: the `ffmpeg` submodule inside `extern/ppsspp`
  is not initialised. The message gives the command; "Get the sources" above
  initialises everything the build needs.
- `pass -Dminibox_dir=<miniBox checkout>` from meson: the native configure
  was given no miniBox and there is none at
  `../chimera/extern/chimera-common-minibox`.
- `no libminiboxhost.so under ...`: miniBox is not built. See "Build miniBox".
- `miniBox C++ guest toolchain missing under .../build/meson-cpp`: the same;
  it must be the build with `-Dguest_cpp=true`.
- FFmpeg fails to configure or build: the script prints the end of
  `waterbox/obj-<flavour>/ffmpeg-build/configure.log` or `make.log`.
- FFmpeg and the math library are built once and then reported as
  `already built`. To rebuild them, for example after the guest toolchain's
  flags changed, remove `waterbox/obj-guest` or `waterbox/obj-native` and
  configure again.
- A change to a patch has no effect: `apply-patches.sh` skips a patch that
  does not apply, without a message. Check the submodule's working tree.
- `FAIL <name> (missing: ... - the pinned test set has moved)`: the
  `pspautotests` submodule is not initialised, or a pin bump moved a path.
- The frontend gate says `native reference not built`: build
  `build/meson-native` first; the gate compares against `run-native`.
- `Xvfb not found`: install `xvfb`. The frontend gate starts its own display
  when `DISPLAY` is not set.
