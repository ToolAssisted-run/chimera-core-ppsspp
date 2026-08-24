# chimera-core-ppsspp

**PPSSPP as a Chimera waterbox core** - the [PPSSPP](https://github.com/hrydgard/ppsspp)
Sony PSP emulator, compiled into miniBox's deterministic sandbox and packaged as a
Chimera core (`core.wbx` + `waterbox.config`), the same shape as
[chimera-core-quickernes](https://github.com/ToolAssisted-run/chimera-core-quickernes) and
[chimera-core-neshawk](https://github.com/ToolAssisted-run/chimera-core-neshawk).

Status: **bring-up**. See [`docs/PLAN.md`](docs/PLAN.md) for the architecture, the
lessons inherited from the previous attempts, and the milestone list.

## Credits & provenance

All emulation comes from **PPSSPP**, by Henrik Rydgård and contributors, GPL-2.0-or-later,
vendored unmodified as the submodule [`extern/ppsspp`](extern/ppsspp) (pinned to a release
tag). The integration layer in `waterbox/` is this repository's own work, under the same
GPL-2.0-or-later license. Earlier integration attempts this work draws on:

- the native (non-waterboxed) BizHawk port and its C# interface
  (TASEmulators/BizHawk @ 09029d8d, `ExternalCoreProjects/ppsspp`),
- the first waterbox attempt (SergioMartin86/BizHawk branch `ppsspp_wbx`,
  `waterbox/ppsspp`, guest built from TASEmulators/ppsspp branch `wbx`),
- headlessPPSSPP (SergioMartin86/headlessPPSSPP).

## Layout

| Path | Contents |
|---|---|
| `extern/ppsspp` | upstream PPSSPP, pinned, unmodified |
| `patches/` | the (small) patch series applied onto the submodule at build time |
| `waterbox/` | the integration layer: driver, guest ABI adapter, build scripts, gate |
| `docs/` | the porting plan and design notes |

## Building

```sh
cd waterbox
./build-native.sh          # host build of the curated source set + driver (the reference)
./build-core.sh            # core.wbx via miniBox's C++ guest toolchain
./build-package.sh -r <chimera checkout>   # -> <chimera>/build/Cores/ppsspp.zip
```

The C++ guest toolchain comes from a miniBox checkout built with
`meson setup build/meson-cpp -Dguest_cpp=true && ninja -C build/meson-cpp`.
