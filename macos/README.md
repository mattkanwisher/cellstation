# CellStation — macOS frontend

A native macOS frontend over the **same** `rpcs3_emu` core the Android app
(CellStation) uses, for fast local iteration on the CPU-side C++ / HLE work
(native incremental builds, real audio, savestates) instead of the slow Android
cross-compile + device-deploy loop.

This is a **self-owned path**: it does **not** use or link the upstream rpcs3
desktop Qt frontend (rpcs3qt). We build only the emulator core `rpcs3_emu` from
the pinned `rpcs3/` submodule + our `patches/`, wrapped in a thin C-ABI bridge
(`macos/bridge/`) — the macOS analogue of the Android JNI bridge.

> Qt6 is required only at **configure time**: rpcs3's monolithic top-level
> CMake pulls Qt in on non-Android hosts. We sidestep the app entirely by
> embedding only rpcs3's `3rdparty` and `Emu` subdirectories (see
> `macos/CMakeLists.txt`), so Qt is never actually needed — but installing it
> keeps you closest to a stock rpcs3 checkout should you ever configure that
> too. Our build links **zero** Qt.

## Status

| Milestone | State |
|---|---|
| **M1** — core builds natively (arm64) | see `PROGRESS.md` |
| **M2** — C-ABI bridge + headless boot harness | see `PROGRESS.md` |
| **M3** — AppKit window + Metal/MoltenVK surface, live input | designed, not built |
| **M4** — library/boot UI resembling the Android app | designed, not built |

`PROGRESS.md` is the source of truth for what currently builds and runs.

## Prerequisites (Homebrew)

```sh
brew install cmake ninja ccache llvm qt ffmpeg
# llvm: rpcs3 needs clang >= 19; Apple clang is too old. We use Homebrew clang.
# qt:   configure-time only (see note above).
# ffmpeg: 8.x satisfies rpcs3's >= 7.1 floor; used via USE_SYSTEM_FFMPEG=ON.
```

Submodules (once): the `rpcs3/` submodule + its 3rdparty submodules, minus the
heavy/unneeded ones (llvm, ffmpeg, MoltenVK, opencv are skipped for the headless
interpreter bring-up):

```sh
git submodule update --init --depth 1 rpcs3
cd rpcs3
for m in $(git submodule status | awk '{print $2}' | grep -vE 'llvm|ffmpeg|MoltenVK|opencv'); do
    git submodule update --init --depth 1 "$m"
done
cd ..
```

## Build

```sh
sh macos/build-macos.sh
```

This resets + re-applies `patches/` to the submodule (the series is not
idempotent), configures `build-macos/` with Homebrew clang for arm64, and builds
the `cellstation_harness` target (which pulls in `rpcs3_emu` + our bridge).

Manual equivalent:

```sh
git -C rpcs3 checkout -- . && git -C rpcs3 clean -fd && sh ci/apply-patches.sh
cmake -S macos -B build-macos -G Ninja \
  -DCMAKE_C_COMPILER="$(brew --prefix llvm)/bin/clang" \
  -DCMAKE_CXX_COMPILER="$(brew --prefix llvm)/bin/clang++" \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-macos --target cellstation_harness -j$(sysctl -n hw.ncpu)
```

Build configuration (headless bring-up): `WITH_LLVM=OFF` (interpreter — the
reference execution path, LLVM-free and fast to build; perf work stays on the
Snapdragon device), `USE_VULKAN=OFF` (Null renderer), `USE_SDL/FAUDIO/…=OFF`.
Flip `-DCELLSTATION_WITH_VULKAN=ON` for the M3 Metal path (needs `brew install
molten-vk vulkan-headers vulkan-loader`).

## Run (M2 headless harness)

The smallest real payload is rpcs3's own homebrew, shipped in the submodule:

```sh
BIN=build-macos/bridge/cellstation_harness   # or build-macos/bin/
# Boot the tetris homebrew headless into an isolated app-data dir, run 8s, exit:
"$BIN" --data-dir /tmp/cellstation-data \
       --boot rpcs3/bin/test/gs_gcm_tetris.elf --run-seconds 8
```

Look for `Boot OK: …` (the stable success marker, same string CI asserts on the
Android side). A commercial ISO additionally needs PS3 firmware + keys; install
firmware with the `fw <PUP>` command.

Without `--boot`, the harness drops into a **command REPL** whose verbs mirror
the scripting vocabulary below (`help` lists them).

## Scripting vocabulary

Automated testing is a first-class requirement: the macOS app is driven from the
outside the way the Android app is driven over adb (DEBUG_PAD broadcasts +
screencap + logcat). The **primary** path (M3/M4) is standard macOS app
scripting — an `.sdef` + `NSScriptCommand` handlers — so a shell can drive it:

```applescript
tell application "CellStationMac"
    boot "/path/to/game"
    press button "cross" hold 250      -- most important: menu/fight navigation
    hold button "r2"
    release button "r2"
    save state to slot 1
    load state from slot 1             -- savestate + scripted input = jump to a fight
    capture screenshot to "/tmp/frame.png"
    get fps
    get status
    reset
    quit
end tell
```

Button names (match the Android pad layout / `macos/bridge/macos_pad.h`):
`cross circle square triangle l1 r1 l2 r2 l3 r3 start select ps up down left right`.

Until the windowed app lands, the **same verbs** are available through the
harness REPL (the documented thin fallback), each mapping to one bridge call:

| REPL command | Bridge call | AppleScript equivalent |
|---|---|---|
| `boot <path>` | `cs_boot` | `boot "<path>"` |
| `press <button> [ms]` | `cs_set_pad_state` | `press button "<b>" hold <ms>` |
| `hold <button>` / `release <button>` | `cs_set_pad_state` | `hold`/`release button "<b>"` |
| `pad <b0,b1,…>` | `cs_set_pad_state` | raw snapshot |
| `save <slot>` / `load <slot>` | `cs_save_state` / `cs_load_state` | `save/load state … slot N` |
| `status` / `fps` | `cs_status_text` / `cs_get_fps` | `get status` / `get fps` |
| `reset` / `kill` / `pause` / `resume` | `cs_reset` / `cs_kill` / … | `reset` / `quit` |
| `fw <pup>` / `fwver` / `version` | `cs_install_firmware` / … | — |

The REPL reads stdin, so scripts pipe commands:

```sh
printf 'boot rpcs3/bin/test/gs_gcm_tetris.elf\nstatus\npress start 200\nsave 1\nquit\n' \
  | "$BIN" --data-dir /tmp/cellstation-data
```

## Layout

```
macos/
  CMakeLists.txt          superproject: embeds rpcs3 3rdparty + Emu (no Qt/app) + bridge
  build-macos.sh          reset+patch submodule, configure, build
  bridge/
    cellstation_bridge.h  the C ABI (every emulator verb; shared by harness + future app)
    cellstation_bridge.cpp bridge impl: EmuCallbacks, main-thread pump, boot, pad, savestates
    core_compat_macos.cpp embedder obligations rpcs3 expects (JNI-free port of core_compat.cpp)
    macos_pad.h           pad snapshot byte layout, mirrors the Android app
    CMakeLists.txt        bridge static lib + cellstation_harness
  harness/
    main.cpp              M2 CLI: boot headless + stdin command REPL (scripting fallback)
  README.md  PROGRESS.md
```

## Architecture notes

- The bridge reproduces the Android bridge contract (`native/bridge/`):
  `cs_initialize` (data dir + user), `cs_boot`, `cs_kill/pause/resume/reset`,
  `cs_set_pad_state/connected`, `cs_install_firmware`, `cs_save_state/load_state`,
  `cs_surface_changed` (M3), `cs_capture_screenshot` (M3), status/version.
- `EmuCallbacks` mirror `chrysalis_boot.cpp`: Null renderer, cubeb (CoreAudio)
  audio with Null fallback, Null keyboard/mouse, upstream `pad_thread`. All
  `std::function` members are set — the tree is `-fno-exceptions`, so an empty
  one aborts via `bad_function_call`.
- `cs_run_main_loop()` drains the core's `call_from_main_thread` queue on the
  thread you designate as "main" (the harness uses the real main thread; a
  control thread issues `cs_boot`/verbs). This is the macOS analogue of the
  Android `runMainLoop`. On the AppKit app (M3) this cooperates with the Cocoa
  run loop.
- Data dir: `--data-dir DIR` points `HOME` at `DIR` so config/cache nest under
  `DIR/Library/Application Support/rpcs3`. NOTE: a global static in the core can
  cache the config dir during process init, before `cs_initialize` runs, so for
  guaranteed isolation set `HOME` in the environment before launching instead:
  `HOME=/tmp/cs-data build-macos/bridge/cellstation_harness --boot …`. With no
  override the default `~/Library/Application Support/rpcs3` is used (fine for a
  dev box).
- Homebrew is used for `ffmpeg` (system), `curl`/`zlib`/`opencv` (system, per
  rpcs3's macOS defaults); everything else is the vendored submodule.

## M3 / M4 plan (designed, not yet built)

- **Metal surface**: build with `-DCELLSTATION_WITH_VULKAN=ON`; create a
  `CAMetalLayer`-backed `NSView`, pass the layer to `cs_surface_changed`, and in
  the bridge create a `VkSurfaceKHR` via `vkCreateMetalSurfaceEXT` (MoltenVK) for
  rpcs3's VK backend. `patches/0004` already fixes the VK WSI stub (Android),
  and the macOS surface path is small and self-contained.
- **Input**: a `macos_pad_handler : PadHandlerBase` that reads
  `cellstation::current_pad_state()` each poll (the storage seam is already in
  place) and registers with `pad_thread`, so scripted/keyboard input reaches the
  emulated pad. This is the macOS analogue of `android_pad_handler`.
- **Scripting**: an `.sdef` + `NSScriptCommand` subclasses on the app delegate,
  each forwarding to a `cs_*` call. The verbs and button names above are the
  contract.
- **Screenshot**: `cs_capture_screenshot` reads back the last presented Metal
  drawable to a PNG (Null renderer has no frame, hence M3).
```
