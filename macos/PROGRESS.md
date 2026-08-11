# CellStation macOS — progress log

Running record of what builds, what doesn't, and exact repro steps. Newest
notes at the top of each section.

## Milestone status

- **M1 — core builds natively (arm64): DONE (GREEN).** `librpcs3_emu.a` +
  `libcellstation_bridge.a` + `cellstation_harness` all build and link with
  Homebrew clang 21 for arm64 (interpreter / `WITH_LLVM=OFF`, Null renderer).
- **M2 — C-ABI bridge + headless boot harness: DONE (GREEN).** The harness boots
  rpcs3's own homebrew headless and the core reports success:
  - `gs_gcm_tetris.elf` → `Boot OK`, `boot result: 0 (no_errors)`, exit 0.
  - Two payloads booted back-to-back in one process (`tetris` then
    `gs_gcm_hello_world`) both return `no_errors` — in-process re-boot works.
  - Cubeb/CoreAudio audio backend initializes (real audio path), Null renderer,
    upstream `pad_thread` with a Null keyboard fallback.
- **M3 — AppKit window + Metal surface + live input:** designed, not built (ABI
  and seams in place; see README "M3/M4 plan").
- **M4 — library/boot UI:** designed, not built.

### Verified run
```
build-macos/bridge/cellstation_harness --boot rpcs3/bin/test/gs_gcm_tetris.elf --run-seconds 5
# -> [+] Boot OK: rpcs3/bin/test/gs_gcm_tetris.elf
#    boot result: 0 (no_errors)   ; exit 0
```

## How it's wired (M1)

We do NOT build rpcs3's Qt app. `macos/CMakeLists.txt` is a Qt-free top-level
that reproduces the subset of rpcs3's top-level + app CMake the core needs, then
embeds only rpcs3's `3rdparty` and `Emu` (rpcs3_emu) subdirectories, plus our
`bridge/`. Build config: `WITH_LLVM=OFF` (interpreter), `USE_VULKAN=OFF` (Null
renderer), `USE_SDL/FAUDIO/DISCORD/GAMEMODE/LTO/PCH=OFF`, `USE_SYSTEM_FFMPEG=ON`,
`USE_SYSTEM_OPENAL=ON`. Compiler: Homebrew clang 21 (Apple clang 17 is too old
for rpcs3's clang>=19 gate). Target: `cellstation_harness`.

### Prereqs confirmed on this machine
- cmake 3.29, ninja 1.11, ccache, Homebrew clang 21.1 (`/opt/homebrew/opt/llvm`).
- Qt6, ffmpeg 8.0, opencv 4.12 present via Homebrew (Qt is configure-time only).
- rpcs3 submodule at pinned SHA 652cf60; 3rdparty submodules initialised except
  llvm / ffmpeg / MoltenVK / opencv (not needed for the headless interpreter).
- All 4 patches in `patches/series` apply cleanly to the pinned SHA.

### Repro
```sh
sh macos/build-macos.sh          # reset+patch submodule, configure, build
# or the manual cmake invocation in README.md
```

## Fixes applied during bring-up

1. **Embedding vs Qt.** rpcs3's top-level CMake forces `find_package(Qt6)` and
   builds rpcs3qt on non-Android hosts, and its app `CMakeLists.txt` uses
   `${CMAKE_SOURCE_DIR}` (→ our macos dir when embedded) to include
   `qt6.cmake`. Fixed by NOT add_subdirectory'ing the whole rpcs3 tree; instead
   embed only `3rdparty` + `Emu` and supply the top-level glue ourselves
   (options, `ConfigureCompiler`, git-version, `CMAKE_CXX_STANDARD 23`,
   Homebrew prefix path). Neither embedded subdir uses `CMAKE_SOURCE_DIR`.

2. **FindZLIB infinite recursion.** `FindZLIB.cmake` (USE_SYSTEM_ZLIB path)
   temporarily `list(REMOVE_ITEM CMAKE_MODULE_PATH <its own dir>)` to reach the
   system module; the match must be exact. Our `RPCS3_DIR` contained `..`, so
   `CMAKE_MODULE_PATH` didn't string-match the normalized `CMAKE_CURRENT_LIST_DIR`
   → find_package recursed to depth 500. Fixed by normalizing `RPCS3_DIR` with
   `get_filename_component(... ABSOLUTE)`.

3. **openal-soft vs Homebrew fmt.** openal-soft bundles `fmt-11.2.0`, but a
   Homebrew-found backend put `-I/opt/homebrew/include` (a different fmt, rejects
   `std::span` formatting) ahead of the bundled one on the `OpenAL` target, so
   `<fmt/base.h>` resolved to Homebrew's and broke `hrtf_loader.cpp`. rpcs3 needs
   openal-soft's bare headers (e.g. `alext.h`) which the system framework lacks,
   so we keep the vendored build but (a) prepend the bundled fmt include to the
   openal targets (`BEFORE PRIVATE`), and (b) set `ALSOFT_RTKIT=OFF` (its DBus
   find was one Homebrew-include source). `USE_SYSTEM_OPENAL=OFF`.

4. **pad_thread wants the Qt keyboard handler.** `rpcs3_emu` references
   `pad::get_pad_thread()` (defined in `Input/pad_thread.cpp`, which the embedder
   compiles), and that file `#include`s the Qt `keyboard_pad_handler` on
   non-Android (`QWindow`). We link no Qt. Fixed with a **macOS-only** patch,
   `macos/patches/0001-pad-thread-headless-keyboard.patch` (applied by
   `build-macos.sh` on top of the shared series, never touching `patches/`),
   gating the keyboard handler behind `CELLSTATION_NO_QT_KEYBOARD` — the same
   Null-pad fallback Android uses. The bridge compiles pad_thread.cpp with that
   define.

## Open items / next

- **Savestates (implemented, NOT yet working):** `cs_save_state` posts
  `Emu.Kill(false, true)` then copies the produced `.SAVESTAT` to
  `savestates/cellstation_slot_N.SAVESTAT`; `cs_load_state` boots it. Against a
  just-booted homebrew the save **hangs** (the Kill-with-savestate path doesn't
  reach `IsStopped(true)` in this headless setup). Needs investigation — likely
  the savestate must be requested from a running, savestateable title, and the
  wait should be bounded. Treat as M3 work; the boot path (M2) is unaffected.
- **`--data-dir` isolation** is best-effort (a core static caches the config dir
  during process init); set `HOME` in the environment for guaranteed isolation
  (see README).
- **Process exit** uses `std::_Exit(0)` after a verified-clean stop, to skip the
  core's global fxo typemap destructor, which asserts it was torn down through
  its own path (standard emulator shutdown pattern). In-process re-boot works.
- **M3:** `-DCELLSTATION_WITH_VULKAN=ON` + `CAMetalLayer` + `vkCreateMetalSurfaceEXT`
  (wire into `cs_surface_changed`); a `macos_pad_handler : PadHandlerBase` reading
  `cellstation::current_pad_state()` (storage seam already present) registered
  with `pad_thread`; `.sdef` + `NSScriptCommand` handlers forwarding to the `cs_*`
  verbs; `cs_capture_screenshot` reading back the Metal drawable.
