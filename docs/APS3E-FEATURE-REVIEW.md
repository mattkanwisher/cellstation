# aPS3e feature review — what they have that we don't, and what's worth taking

Reconnaissance of `aenu1/aps3e`, the other Android RPCS3 derivative, against
CellStation as of 2026-08-07. Purpose: find features worth adopting and decide,
per feature, whether to implement it ourselves or take their code.

## Method and what I could not verify

Everything below comes from reading their published source and metadata. I did
**not** build or run aPS3e, and I did not install it on a device. Specifically I read:

- `README.md`, the wiki FAQ, the GitHub release/tag/commit history
- `app/src/main/res/xml/emulator_settings.xml` (their whole settings tree, 238 keys)
- `app/src/main/AndroidManifest.xml`, `res/menu/*`, the relevant `res/layout/*`
- the Java sources under `app/src/main/java/aenu/aps3e/` (~330 KB)
- `app/src/main/cpp/{CMakeLists.txt,aps3e_emu.cpp,vkapi.cpp}` and the JNI declarations
- `AboutActivity.java`, which embeds their full changelog from 0.1 to 1.38

Known gaps in this review, stated plainly:

- **Their published source stops at 1.38 (2026-04-13).** Play Store releases 2.39,
  2.40 and 2.41 (May–July 2026) exist as GitHub tags, but all three point at the
  same April 2026 commit (`b5ae1af`), and the in-app changelog also ends at 1.38.
  I cannot tell what changed in the 2.x builds that are actually shipping. Treat
  any claim about current aPS3e behaviour as "as of 1.38".
- **I did not diff their vendored RPCS3 tree against upstream.** They vendor the
  entire RPCS3 source at `app/src/main/cpp/rpcs3/` rather than tracking it as a
  submodule, so their core divergence is not visible as a patch series. Producing
  that diff is a separate, worthwhile task (see "Follow-up work"); the core
  divergences listed here are only the ones I could infer from config keys, JNI
  signatures and the changelog.
- Their wiki FAQ is stale in places (it still says there is no cheat support; a
  memory search/write feature landed in 1.34).
- `aps3e.org` turns up in search results distributing APKs. Their own FAQ names
  `aenu.cc/aps3e` and the Play Store as the official channels. I did not verify
  what `aps3e.org` serves and would not treat it as a source of anything.

## Licensing and attribution posture

- Their own C++ files carry `SPDX-License-Identifier: GPL-2.0-only`
  (e.g. `app/src/main/cpp/aps3e_emu.cpp`). At least one Java file
  (`Application.java`) is marked WTFPL.
- The vendored RPCS3 tree retains upstream's `LICENSE` and `README.md`.
- There is **no top-level LICENSE file**; GitHub reports no licence for the repo,
  and the README says to "check the LICENSE file under the appropriate file header
  and directory". The About screen has an "open source licenses" dialog listing
  RPCS3, LLVM, ffmpeg, glslang, libadrenotools, wolfssl, Vita3K-Android and others.
- Practical upshot: **their code is GPL-2.0, same as ours, so taking it is legally
  permissible** provided we retain notices and record provenance. It is not
  "public domain because it's on GitHub".

My recommendation throughout is **implement ourselves, not copy**, for two reasons
that are about us rather than about them:

1. Our value proposition is a clean, upstreamable patch series against a pinned
   submodule (`docs/PATCH-INVENTORY.md`). Importing chunks of a vendored fork
   works against that — we would be inheriting code we can't cleanly rebase and
   can't offer upstream.
2. Almost everything interesting they have is *frontend* work in the Kotlin/Java
   layer, where their code is Java-with-framework-views against a different app
   architecture. There is nothing to copy that would be less work than writing it.

The one place where copying may genuinely be cleaner is the Mali/Valhall Vulkan
workaround set (see P2-17). If we take that, it goes in as a patch with the
provenance in the patch header **and** an entry in `docs/ATTRIBUTION.md`. Even for
features we write ourselves after reading their code closely, I'd add an
"inspired by" line to `ATTRIBUTION.md` — cheap, and our patch series is public.

## What aPS3e is

An Android PS3 emulator by a developer using the handle `aenu`, first released
2025-01-06, built by vendoring RPCS3 into an Android Studio project and adding a
Java frontend. Distributed on Google Play in a free and a "donation" build (their
FAQ: no functional difference, only the icon). ~700 stars, ~100 open issues.
Android glue appears to borrow from Vita3K-Android (credited in their licence list;
their `DocumentsProvider` is recognisably that lineage).

Architecture notes that differ from ours:

- **Vendored core, not a submodule.** No patch series; RPCS3 changes are edited
  in place.
- **Emulation runs in a separate process** (`android:process=":emu"` on
  `EmulatorActivity`). The UI process survives an emulator crash and each game
  gets a fresh address space.
- **No `MANAGE_EXTERNAL_STORAGE`.** Data lives in `getExternalFilesDir("aps3e")`,
  exposed to the system file picker through their own `DocumentsProvider`; game
  directories are added as SAF tree URIs. Play-distribution-compatible by
  construction.
- **No foreground service.** The `PPUCacheBuildService` and its manifest entry are
  commented out — the changelog says the "create PPU cache" feature was removed in
  1.35 because Google Play rejected it.
- **Settings UI is generated from the core's config tree.** They have native
  methods `generate_config_xml()`, `generate_strings_xml()` and
  `generate_java_string_arr()`, and their `emulator_settings.xml` uses keys of the
  form `Section|Setting` (`Core|SPU Block Size`, `Video|Vulkan|Adapter`, …) that
  map 1:1 onto RPCS3 config paths. This is why they can expose 238 settings without
  hand-writing 238 rows.

## Their user-facing feature inventory

### Settings (238 keys, near-complete mirror of the RPCS3 config tree)

Screens: Core, Video (incl. Video|Vulkan, Video|Vulkan|Debug, Video|Performance
Overlay, Video|Shader Loading Dialog), Audio, Input/Output, System, Savestate,
Miscellaneous.

Config keys they expose that **do not exist in upstream RPCS3** at our pin
(`652cf60`) — i.e. these are their own core additions, verified by grepping our
submodule:

| Key | What it is |
|---|---|
| `Video/Use BGRA Format` | Adreno colour-order workaround; default on since 1.23 |
| `Video/Force Convert Texture` | texture conversion workaround |
| `Video/Texture Upload Mode` | added 1.22, "fix Adreno 7xx stock driver crash" by defaulting to CPU texture handling |
| `Video/Vertex Buffer Upload Mode` | added 1.13 after vertex buffer hangs |
| `Video/Vulkan/Use Custom Driver` | adrenotools toggle |
| `Video/Vulkan/Custom Driver Library Path` | driver picker |
| `Video/Vulkan/Custom Driver Force Max Clocks` | adrenotools turbo |
| `Core/Thread Affinity Mask` | per-core checkbox mask (`res/layout/setting_affinity_mask.xml`) |
| `Miscellaneous/Font File Selection`, `Custom Font File Path`, `Font Size` | overlay font override; added after an Android 15 font crash |
| `Miscellaneous/Memory Debug overlay` | extra debug overlay |

Everything else on their settings screens is upstream RPCS3 config, surfaced verbatim.

### Screens and flows

- **Quick start wizard** — Vulkan support check, firmware install, ISO directory,
  custom driver path, custom font path.
- **Game list** — icon list, cached to a JSON file, context menu per game
  (see below), DLC/update info per serial, trophy dialog.
- **Per-game context menu** — delete game + data, delete game data, delete
  hdd0 install data, delete shader cache, delete PPU cache, delete SPU cache,
  edit custom config, create launcher shortcut, show game info, show trophy info.
- **User data manager** — a hub with five pages: PPU cache manager
  (export a serial's PPU cache to a zip, import one back), ISO directory manager,
  compatibility table (downloads and caches it), GPU driver manager (list, inspect,
  delete installed drivers), game data manager (export/import
  `config/dev_hdd0/home/<user>` as a zip — i.e. save backup/restore).
- **Key mapper** — physical pad remapping, plus a vibration enable toggle and a
  vibration duration slider.
- **Virtual pad editor** — per-group size sliders (stick, d-pad, face buttons,
  start/select, L/R, PS) each 20–300 %, a dynamic/floating joystick mode
  (disabled / left / right), reset to default, and a global "disable virtual pad".
- **In-game menu** (back button) — only two entries: **memory search** and **quit**.
  Everything else in-game goes through RPCS3's own Home Menu overlay, reachable
  because their virtual pad has a PS button.
- **Memory search/write** — a cheat-engine-style search over emulated memory with
  typed values, result list, and write-to-address. JNI: `search_memory`,
  `set_cheat`, `get_cheat`.
- **About** — device info, changelog, third-party licence dialog, contributor list,
  and an **enable-log** checkbox (logs go to `<data>/logs`, retrievable through the
  DocumentsProvider).

### Content handling

PKG install (`install_pkg(fd)`), EDAT install (`install_edat(fd)`), firmware PUP
install (`install_firmware(fd)`), ISO / folder / disc-game boot, automatic PKG
install when opening a disc-game-class title (1.31), refusal to boot undecrypted
games with an explicit message, trophy data read from disc
(`trophy_info_from_dir`), and camera frame injection (`camera_frame(byte[], w, h,
format)`, added April 2026).

### Core-side additions visible through JNI

`Emu.PrecompilePPUCache(path, fd)` — **not present upstream**; a headless
"compile the PPU cache without booting" entry point, used by the (now removed)
cache builder and still used for the export/import flow. Also
`get_support_llvm_cpu_list` / `get_native_llvm_cpu_list` (LLVM `-mcpu` selection),
`get_vulkan_physical_dev_list`, and CPU topology getters.

### Platform

21 locales (`values-ar` … `values-zh-rTW`), including a mechanism that pulls
RPCS3's own overlay/Home-Menu strings (`HOME_MENU_SAVESTATE`, `SAVESTATE_FAILED_*`, …)
into the Android string table so the core's UI is translated too. Non-touchscreen
devices supported (1.37). Startup Vulkan capability probe. Device-specific
workaround: on Adreno 5xx/6xx the native library load is delayed 0.5 s because
loading it immediately prevented startup (1.28).

## Where we already match or lead

Checked against our tree, not assumed:

| Area | Status |
|---|---|
| Per-game config overrides | Both. Ours: 5 curated keys + generic bridge. Theirs: full tree. |
| Custom GPU driver (adrenotools) | Both, same library. They add a driver manager UI and a "force max clocks" toggle; our `GpuDriver.uninstall()` exists but is unwired. |
| Core RSX performance overlay | Both expose it. Ours: 6 rows + an in-game toggle. Theirs: all 17 keys. |
| Boot progress UI | **We lead.** `BootProgressView` shows the core's phase text, a determinate bar and an ETA. They rely on the core's shader loading dialog. |
| Host CPU/GPU HUD | **We lead.** `HostStatsView` reads `/proc/self/stat` and kgsl. No equivalent. |
| ADPF performance hints | **We lead.** No sign of ADPF in their tree. |
| Big-core affinity | Both, differently. Ours is automatic (patch 0017, frequency-table classification). Theirs is a manual per-core mask. |
| Compatibility data + box art | **We lead.** We fetch the RPCS3 community export *and* GameTDB covers; they fetch the compatibility table only. |
| Handheld device profiles | **We lead.** AYN/Retroid/Anbernic detection with dynamic face-button layout. No equivalent. |
| Frontend launch intents | **We lead.** Documented contract in `docs/INTENTS.md` for Daijishō/ES-DE/Cocoon. They have `ACTION_VIEW` and a private action, undocumented. |
| Library frontend polish | **We lead.** Icon grid, live search, Continue card, hide-game, homebrew tab, d-pad focus. Theirs is a list with a context menu. |
| Patch discipline | **We lead.** 17 numbered patches against a pinned submodule vs. an in-place vendored tree. |

## Adoption plan

Layer key: **app** = Kotlin only, **bridge** = `native/bridge/bridge.cpp` (+ Kotlin),
**patch** = new entry in `patches/series`. Effort is calendar-rough for one person:
S ≈ under a day, M ≈ a few days, L ≈ a week or more.

### P0 — real gaps that cost us correctness or supportability

**P0-1. Generic settings tree, generated from the core's config.**
*What:* stop hand-writing setting rows. Add a bridge call that walks the `cfg::node`
tree and returns the whole thing (path, type, current value, options, min/max) as
JSON; render it generically in Kotlin as a browsable tree; keep the existing curated
"Quick" page as the front door. We already have `globalConfigGet/Set/Options` and
`gameConfigGet/Set/Options` for arbitrary paths — the bridge is 80 % there, we just
have no enumeration call and no generic renderer.
*Why:* we expose 6 global and 5 per-game keys. Upstream has 272. The ones we are
missing that users actually need are not exotic: `Video/Resolution Scale`,
`Video/Frame limit`, `Video/VSync`, `Video/Shader Mode`, `Video/Write Depth Buffer`,
`Video/Disable ZCull Occlusion Queries`, `Video/Anisotropic Filter Override`,
`Audio/Master Volume`, `Audio/Enable Buffering` + `Desired Audio Buffer Duration`,
`System/Language`, `System/Enter button assignment`, `Core/Preferred SPU Threads`,
`Core/Max LLVM Compile Threads`, `Core/Libraries Control`. Right now the only way to
change any of these is to hand-edit YAML. This is the single highest-leverage item
on the list and it unblocks per-game tuning for everything else.
*Layer:* bridge (one enumeration call) + app (generic renderer, ~1 screen).
*Effort:* M. *Copy or implement:* implement. Their *technique* — machine-generating
the settings surface from the cfg tree — is the idea worth taking; their XML
generator is not.

**P0-2. Save-data dialog (`get_save_dialog`).**
*What:* implement `SaveDialogBase` so `cellSaveData` list dialogs work.
*Why:* we currently return null, which makes the list dialog return `CELL_CANCEL`.
Any title that presents a save-slot picker cannot save or load. This is a
correctness bug that looks like a game bug. aPS3e implemented this back in 0.11.
*Layer:* bridge (+ app, or render through the core's overlay path like msg/OSK).
*Effort:* M. *Copy or implement:* implement — but read the upstream Qt implementation
first, it is the reference, not theirs.

**P0-3. PKG installation (and EDAT/RAP).**
*What:* wire `on_install_pkgs` to upstream's `package_reader`, add a picker and a
progress UI, and add EDAT/licence install.
*Why:* `on_install_pkgs` returns `false` today, so we cannot install updates, DLC,
or any PSN-format content, including a lot of homebrew. It is also the path aPS3e
uses to make disc-class titles work correctly (their 1.31 auto-install behaviour).
*Layer:* bridge + app. *Effort:* M. *Copy or implement:* implement; upstream has the
reader, this is plumbing.

**P0-4. In-app log capture and export.**
*What:* add a file sink alongside the existing logcat `logs::listener`, a toggle,
and a "share log" action.
*Why:* we are logcat-only. For a sideloaded emulator this is the number one support
tool — without it every bug report is unactionable. aPS3e writes to `<data>/logs`
and exposes it through their DocumentsProvider; even a share-sheet export would do.
*Layer:* bridge (sink) + app (toggle, share). *Effort:* S. *Copy or implement:* implement.

### P1 — clear feature gaps, app-side, worth doing next

**P1-5. Touch overlay: sticks, L3/R3, PS button, editor, haptics.**
*What:* add analog sticks (including a floating/dynamic stick anchored where the
thumb lands), L3/R3, a PS button, per-group scale sliders, a layout editor, and
optional vibration on press.
*Why:* our `TouchOverlayView` has d-pad, face buttons, L1/L2/R1/R2, Select and Start
— no sticks at all, which makes most 3D titles unplayable without a physical pad,
and no PS button, which means touch-only users cannot reach the core's Home Menu
(where Exit/Restart/Trophies/Screenshot live). aPS3e's dynamic-joystick idea is
good and cheap.
*Layer:* app only. *Effort:* M–L (the editor is most of it; sticks + PS button alone
are S–M and should ship first). *Copy or implement:* implement.

**P1-6. Trophy notifications and a trophy viewer.**
*What:* implement `get_trophy_notification_dialog`; add a per-game "trophy info"
sheet reading the trophy data on disc.
*Why:* ours is null, so trophies unlock silently. Low risk, visible polish.
*Layer:* bridge + app. *Effort:* S–M. *Copy or implement:* implement.

**P1-7. Granular cache management + PPU cache export/import.**
*What:* split our single "Clear cache" into PPU / SPU / shader cache, and add
export/import of a serial's PPU cache as a zip.
*Why:* first-boot PPU compilation is the dominant cost in our own benchmarks; being
able to move a warmed cache between devices (or restore it after a wipe) is a real
quality-of-life win, and being able to clear *just* the shader cache after a driver
change without throwing away the PPU cache is something we currently can't do.
Note aPS3e had to remove the *build-ahead* variant to satisfy Play review; the
export/import half survived, which suggests it's the safer shape.
*Layer:* app mostly (we already have `clearGameCache`; needs per-subdirectory
variants). *Effort:* S. *Copy or implement:* implement.

**P1-8. Localisation.**
*What:* extract `strings.xml` properly and add locales; separately, consider
mirroring their trick of translating the *core's* overlay strings.
*Why:* we are English-only with no `values-xx/`. aPS3e ships 21 locales and a large
share of their user base is non-English. The app-string half is nearly free; the
core-string half is a bigger lift and can wait.
*Layer:* app (+ optional patch for the core string table). *Effort:* S for app
strings, M for core strings. *Copy or implement:* implement. Do **not** import their
translations — they're theirs, and quality is unverified.

**P1-9. Per-game launcher shortcuts.**
*What:* `ShortcutManager` pinned shortcut per game, launching our existing
`EMULATE` intent.
*Why:* small, obvious, and it composes with the intent contract we already have.
*Layer:* app. *Effort:* S. *Copy or implement:* implement.

**P1-10. GPU driver manager completion.**
*What:* wire up the existing unused `GpuDriver.uninstall()`, show driver metadata
(name, description, Vulkan version, supported extensions), and evaluate
adrenotools' max-clocks/turbo option behind a clearly-labelled toggle.
*Why:* we install drivers but can't remove them, which is a dead end for anyone
trying several Turnip builds. Their driver manager is the obvious shape.
Max-clocks needs measurement before we ship it — it will affect thermals, and our
own benchmark methodology should decide whether it's a win.
*Layer:* app (+ small bridge change for turbo). *Effort:* S. *Copy or implement:* implement.

### P2 — worth considering, with caveats

**P2-11. Separate `:emu` process for emulation.**
*What:* move `EmulationActivity` into `android:process=":emu"`.
*Why:* crash isolation (the library survives an emulator crash), and every game
launch gets a fresh address space — which is directly relevant to the JIT
address-space behaviour we've already characterised, where reservations accumulate
within a process. Also lets us kill and relaunch cleanly rather than relying on
`Emu.Kill`.
*Caveat:* it complicates state sharing between the library and the emulator (our
`SharedPreferences` reads, boot progress, `GameLibrary`), and interacts with our
foreground-service lmkd strategy. Wants a design pass, not a quick change.
*Layer:* manifest + app. *Effort:* M. *Copy or implement:* implement.

**P2-12. SAF/DocumentsProvider storage model.**
*What:* expose our data dir through a `DocumentsProvider`, and support SAF tree
URIs for game directories, so `MANAGE_EXTERNAL_STORAGE` becomes optional.
*Why:* strategic rather than urgent. It matters if we ever want Play distribution
(where `MANAGE_EXTERNAL_STORAGE` is effectively refused for emulators), and it
insulates us from further scoped-storage tightening. Today our sideload-only
posture makes the all-files permission a reasonable choice.
*Caveat:* the core reads game data through real paths; ISO-from-fd we already
handle, but folder-format games over SAF would need VFS work. This is the reason
aPS3e requires folder games to be copied into their app dir.
*Layer:* app + bridge. *Effort:* L. *Copy or implement:* implement.

**P2-13. Startup Vulkan probe and device gating.**
*What:* probe for a usable Vulkan device before init and show a clear message
instead of failing obscurely; extend `DeviceProfile` with per-GPU workarounds.
*Why:* cheap, and we already have the profile mechanism. Their Adreno 5xx/6xx
delayed-dlopen hack is the kind of thing that belongs in `DeviceProfile` if we ever
see the same symptom — I have not reproduced it and would not add it speculatively.
*Layer:* app + small bridge. *Effort:* S. *Copy or implement:* implement.

**P2-14. Manual thread affinity mask override.**
*What:* a per-core checkbox mask as an escape hatch over patch 0017's automatic
big-core classification.
*Why:* our automatic classification is a better default, but on an SoC whose
frequency table lies, a manual override is the difference between "works" and
"file a bug". Low cost given 0017 already exists.
*Layer:* patch (extend 0017) + app. *Effort:* S.

**P2-15. Camera passthrough.**
*What:* feed Android camera frames into the PS3 camera device.
*Why:* niche — a handful of titles. Only worth it if someone asks.
*Layer:* bridge + app. *Effort:* M.

**P2-16. Memory search / write.**
*What:* a cheat-engine-style search over emulated memory.
*Why:* fun, and their implementation is only a couple of JNI calls plus a dialog.
But it's low value against the rest of this list, and it is exactly the kind of
feature that draws store-policy attention. Park it.
*Layer:* bridge + app. *Effort:* M.

**P2-17. Mali / Valhall Vulkan workarounds.**
*What:* their April 2026 community PR (`o-bin`, merged as `b5ae1af`) touches
`VKGSRender.cpp`, `VKPresent.cpp`, `VulkanAPI.cpp`, `vkutils/buffer_object.cpp`
and `vkutils/instance.cpp` — about 30 lines total — to make Mali-G57 Valhall work.
*Why:* we are Adreno-only today. If we widen device support, this is prior art for
exactly the class of bug we've been fixing on Adreno (patches 0009, 0011, 0014, 0016).
*This is the one item where copying may be cleaner than reinventing*: it's small,
device-specific, and hard to rediscover without the hardware. **If we take it, it
goes in as a patch with provenance in the patch header and an entry in
`docs/ATTRIBUTION.md`**, and only after we can test it on a Mali device.
*Layer:* patch. *Effort:* S to port, unbounded to validate without hardware.

**P2-18. Their non-upstream video workaround options — investigate, don't copy.**
`Video/Use BGRA Format`, `Video/Force Convert Texture`, `Video/Texture Upload Mode`,
`Video/Vertex Buffer Upload Mode` are all Adreno-driven workarounds they added and
then spent several releases fixing regressions in (see 1.21 → 1.23 → 1.27 in their
changelog). We have already solved at least one texture-corruption problem in this
space differently and, I'd argue, more correctly — patch 0016 removed the bogus
`chip_class::unknown` NVIDIA transfer hack rather than adding a toggle. Before
adding any of these, work out what the underlying defect is; a user-visible toggle
for "colours are wrong" is a symptom, not a fix.

### Not recommended

- **Adopting their PPU-cache build-ahead service.** They removed it under store
  pressure; we'd inherit the complexity for a feature whose value is largely covered
  by cache export/import (P1-7).
- **Following their versioning/release practice.** Tags 2.39–2.41 point at an April
  commit while the Play builds are newer. Whatever the explanation, it means we
  cannot treat their repo as a reliable mirror of what ships, and we should not
  model our release process on it.
- **Copying their frontend wholesale.** Our library UI is further along.

## Follow-up work

1. **Diff their vendored `app/src/main/cpp/rpcs3/` against upstream RPCS3 at their
   base commit.** This is the only way to see their real core divergence — the
   custom config keys, `PrecompilePPUCache`, the Adreno workarounds, the memory
   search hooks and whatever else. It's a large checkout but a mechanical job, and
   the result would slot straight into `docs/PATCH-INVENTORY.md` as a second
   comparison column. I did not do it here.
2. **Verify what changed in 2.39–2.41.** Either the source is unpublished or the
   tags are wrong. Worth re-checking periodically; if source for shipping builds
   stays unpublished, note it and move on rather than speculating about it.
3. **Decide the P0-1 settings model before building anything else on this list.**
   Several P1 items (per-game tuning, driver options, audio) get much cheaper once
   there is a generic settings renderer, and much of the hand-written Settings code
   we'd otherwise add would be thrown away.
