# Cocoon frontend integration

How CellStation becomes a launchable PS3 player inside the Cocoon frontend, what
has to happen on the device, and what has to be contributed upstream.

Everything under "Measured state" was read off the AYN Thor over `adb` on
2026-08-07. Nothing in this document has been applied to the device — the setup
sections are instructions, not a change log. Where something is inference rather
than observation it says so.

## What Cocoon is

Cocoon (marketed as **Cocoon Shell**) is a closed-source Android launcher /
emulation frontend in the EmulationStation / Pegasus / Daijishō family. It does
not emulate anything; it organises a game library and hands each game off to a
separate emulator app via an Android intent.

| | |
|---|---|
| App package | `rip.moth.cocoonshell` |
| Docs | <https://cocoon-shell.com/wiki/> |
| Platform database | <https://github.com/inssekt/CocoonFE> (public, PR-friendly) |
| On-device data | `/sdcard/Cocoon data/` (themes, scraped art, debug logs) |

The app itself is closed source, but the part we need is not: the **platform
database is an open repo**. `platforms/index.json` lists 125 platforms and
carries the base URI the app fetches from:

```
https://raw.githubusercontent.com/inssekt/CocoonFE/main/platforms/
```

The app pulls these JSONs from `main` **at runtime** (Settings → Library & data →
Refetch Platforms). A merged PR therefore reaches users without an app update —
this is the whole reason Cocoon integration is cheap for us.

The files are Daijishō-format platform JSONs, so the same player entry works in
Daijishō for free.

## Why this project cares

CellStation is a handheld-first emulator. On a device like the Thor the frontend
*is* the OS as far as the user is concerned — they boot into Cocoon's grid and
may never touch the Android launcher. "Installed but not selectable as a PS3
player in Cocoon" is, in practice, "not installed".

`docs/INTENTS.md` already defines our launch-intent contract, and
`AndroidManifest.xml` already exports `EmulationActivity` with both the custom
action and `ACTION_VIEW` filters. The contract exists; what is missing is the
row in Cocoon's database that points at it.

## How Cocoon actually launches a game

Read off `/sdcard/Cocoon data/launch_debug.log` (Cocoon writes a plaintext trace
of every launch — this is the best available documentation of the mechanism):

```
[INFO] Player: RetroArch (64 bits) - nestopia (com.retroarch.aarch64/...RetroActivityFuture)
[INFO] External display: false
[INFO] URI type: content=true, file=false, path=false
[INFO] Resolved file: /storage/6A60-DFF1/ROMs/NES/<file> (exists=true, hasRealPath=true)
[INFO] {file.uri}  → content://com.android.externalstorage.documents/tree/...
[INFO] {file.path} → /storage/6A60-DFF1/ROMs/NES/<file>
[INFO] Intent: action=android.intent.action.VIEW, component=ComponentInfo{...}
[INFO] Granting tree URI permissions to com.retroarch.aarch64
[INFO] Launch SUCCESS — taskId=null
```

So:

1. Each player entry carries an `amStartArguments` string, which is literally
   `am start` arguments with template placeholders substituted.
2. Placeholders in use across the whole database: `{file.path}` (732 uses),
   `{file.uri}` (130), `{file.mime}` (2), and three tag placeholders —
   `{tags.steamappid}`, `{tags.localgameid}`, `{tags.ps3folder}`.
3. Cocoon resolves the SAF `content://` URI back to a real path where it can,
   and exposes both forms.
4. Before launching, Cocoon **grants tree URI permissions to the target
   package**. A player does not need broad storage permission to read a
   `content://` boot path.
5. `killPackageProcesses` decides whether the previous emulator process is killed
   first.
6. There is a `Swap` action distinct from `Launch`, and an `External display`
   flag — this is the Thor's dual-screen handling. Not yet investigated on our
   side.

### The tag-file mechanism (folder-format games)

PS3 rips are often a `PS3_GAME` folder, not an ISO, and a frontend that scans for
files cannot represent a folder as a library entry. Cocoon's answer is a sidecar
file: a zero-content-relevant file named `<Game Title>.ps3folder`, matched by
`"acceptedFilenameRegex": "^(.*)\\.(?:ps3folder)$"`, with `{tags.ps3folder}`
expanding to the folder it refers to. The same pattern backs Steam
(`.steamappid`) and Windows (`.localgameid`) entries.

Inference, not yet verified on hardware: the exact contents/encoding of a
`.ps3folder` file. Verify against a real one before promising folder support.

## Measured state on the Thor (2026-08-07)

| Check | Result |
|---|---|
| `rip.moth.cocoonshell` installed | yes |
| `nu.hyperworks.cellstation` installed | yes — `0.2.0-dev`, versionCode 1 |
| `aenu.aps3e` installed | yes (the competing PS3 player) |
| PS3 platform registered in Cocoon | **no** |
| SAF grant for a PS3 ROM folder | **no** — 30 persisted grants, none for PS3 |
| `/storage/6A60-DFF1/ROMs/PS3` exists | yes — 5.2 GB, one disc image |
| CellStation listed as a PS3 player | **no** (not in the upstream JSON) |

So the SD card is already laid out for PS3 and the app is already installed; the
two missing pieces are a Cocoon-side platform registration and an upstream
database entry.

## Setup, part 1 — device side (not yet applied)

This is UI work in Cocoon's settings; it cannot be scripted, because SAF grants
must go through the system picker (`adb` cannot inject them — see the ayn-thor
notes). Sequence:

1. `Y` on an **empty grid cell** → **Cocoon Settings** → **Library & data**.
2. Scroll past the options block to the **`Y +`** button above the platform list
   → **Add Platforms** (multi-select, alphabetical). Check **Sony PlayStation
   3**. Do not uncheck anything already checked. Confirm with `Y ✓`.
3. Find *Sony PlayStation 3* in the platform list below, open it, choose **Add
   new ROMs folder**, and in the SAF picker select `ROMs/PS3` on the SD card →
   **Allow**.
4. Cocoon auto-assigns a default emulator. Today the only candidates are the two
   aPS3e entries; CellStation cannot appear until part 2 lands.

Two Cocoon UI traps that apply here, both from the ayn-thor notes: menu rows need
a **double tap** (first focuses, second activates), and entering Settings kills a
running scrape.

Verify the grant landed without touching the UI:

```bash
adb -s 5ea95b5f shell "cat '/sdcard/Cocoon data/permissions_debug.txt'" | grep -i ps3
```

Optionally, add CellStation as a plain app tile on the home grid (`Y` → **Apps**)
so it is reachable before it is selectable as a player. That gives a launcher
tile only, not per-game booting.

## Setup, part 2 — verifying the contract without Cocoon

The launch path can be exercised directly, which is worth doing *before*
submitting anything upstream, because a broken entry in the shared database
affects every Cocoon user. This is exactly what Cocoon will run, minus the URI
grant:

```bash
# ISO form — what the player entry will emit
adb -s 5ea95b5f shell am start \
  -n nu.hyperworks.cellstation/.EmulationActivity \
  -a nu.hyperworks.cellstation.EMULATE \
  -e bootPath "/storage/6A60-DFF1/ROMs/PS3/<image>.iso" \
  --activity-clear-task --activity-clear-top

# content:// form, matching Cocoon's {file.uri} substitution
adb -s 5ea95b5f shell am start \
  -n nu.hyperworks.cellstation/.EmulationActivity \
  -a nu.hyperworks.cellstation.EMULATE \
  -e bootPath "content://com.android.externalstorage.documents/tree/6A60-DFF1%3AROMs%2FPS3/document/6A60-DFF1%3AROMs%2FPS3%2F<image>.iso" \
  --grant-read-uri-permission \
  --activity-clear-task --activity-clear-top

# folder form
adb -s 5ea95b5f shell am start \
  -n nu.hyperworks.cellstation/.EmulationActivity \
  -a nu.hyperworks.cellstation.EMULATE \
  -e gameDir "/storage/6A60-DFF1/ROMs/PS3/<folder>" \
  --activity-clear-task --activity-clear-top
```

Note the asymmetry worth checking: Cocoon grants the URI permission itself before
starting the activity, whereas the `adb` reproduction relies on
`--grant-read-uri-permission`. CellStation also holds
`MANAGE_EXTERNAL_STORAGE`, so the real-path form should work regardless — which
means a `content://` regression could hide behind the path form passing.

## Upstream contribution — required, not optional

**Yes, we have to contribute upstream.** Cocoon's own docs say it "only shows
emulators that are actually installed on your device", and the wiki documents no
way to add a custom player, custom `am start` arguments, or a locally imported
platform JSON — the only knobs are a per-platform **Default Emulator** dropdown
and a per-game **Player** override, both populated from the fetched database. The
base URI is baked into `index.json` on `main`, and the fetched data lands in the
app's private storage, which is unreadable on an unrooted device. There is no
local override path. A player entry in `inssekt/CocoonFE` is the only mechanism.

### What to submit

One file: `platforms/SonyPlayStation3.json`. It is currently
`databaseVersion: 14`, `revisionNumber: 1`, with a `playerList` of exactly two
aPS3e entries (ISO via `-e iso_uri {file.uri}`, folder via
`-e game_dir {tags.ps3folder}`). We append two entries in the same shape:

```json
{
  "name": "CellStation",
  "uniqueId": "ps3.nu.hyperworks.cellstation",
  "description": "Supported extensions: iso.",
  "acceptedFilenameRegex": "^(.*)\\.(?:iso)$",
  "amStartArguments": "-n nu.hyperworks.cellstation/.EmulationActivity\n -a nu.hyperworks.cellstation.EMULATE\n -e bootPath {file.uri}\n --activity-clear-task\n --activity-clear-top\n",
  "killPackageProcesses": true,
  "killPackageProcessesWarning": true,
  "extra": ""
},
{
  "name": "CellStation (Folder Style)",
  "uniqueId": "ps3.nu.hyperworks.cellstation.folder",
  "description": "Supported extensions: ps3folder.",
  "acceptedFilenameRegex": "^(.*)\\.(?:ps3folder)$",
  "amStartArguments": "-n nu.hyperworks.cellstation/.EmulationActivity\n -a nu.hyperworks.cellstation.EMULATE\n -e gameDir {tags.ps3folder}\n",
  "killPackageProcesses": true,
  "killPackageProcessesWarning": true,
  "extra": ""
}
```

Notes on the choices:

- `uniqueId` follows the database convention `<platform>.<package>[.<variant>]`.
- Cocoon appears to derive "is this player installed?" from the package name in
  the `-n` argument (inference, from the closed PR #257, which described exactly
  that failure mode). Getting the component wrong means the entry silently never
  appears.
- `killPackageProcesses: true` matches aPS3e and is the safer default for an
  RPCS3-derived core, which carries process-global state and is not designed to
  boot a second title in the same process.
- The `\n` sequences are part of the JSON string; the database formats
  `amStartArguments` as newline-separated arguments.

### The process, as actually practised

The README points contributors at GitHub Discussions and Discord and documents no
PR procedure — but the merge history says PRs are the real path. Merged examples:
"Add ARMSX2 refresh to PS2 platform JSON" (#134), "Add NetherSX2-Turnip support"
(#128), "Add AzaharPlus to Nintendo3DS.json" (#127), "Add Eden Nightly support"
(#71). Each touched exactly one file under `platforms/`.

Two corrections to what `docs/INTENTS.md` currently claims:

- **`platforms/index.json` does not need touching.** Its history shows it is
  edited only when a *new platform* is added, never for a player addition.
- **Merged player PRs do not bump `revisionNumber`**, in the platform file or in
  the index. #134, #127 and #125 are all single-hunk inserts into `playerList`.
  Follow the observed convention rather than the one we wrote down; a maintainer
  can ask for a bump if they want one.

There is no CI validation on PRs — `.github/workflows/` contains only a
discussion labeller — and no issue/PR template. Validate locally with `jq` or
`python3 -m json.tool` before opening.

The repo has **no LICENSE file**. Worth a moment's thought before contributing,
though for a data-only PR to a database whose entire purpose is redistribution it
is a small concern.

### Timing, and one cautionary data point

PR #257 ("Add OpenSw Nintendo Switch player") was **closed, without comment**. It
was a well-formed, correctly-shaped entry — the plausible reason is that the
emulator was not publicly available. Reading across that and the merged PRs, the
implicit bar is a player users can actually install.

CellStation is `0.2.0-dev` with versionCode 1 and no public release. So:

1. Land a tagged release with a downloadable APK first.
2. Freeze the intent contract at that tag — `docs/INTENTS.md` currently marks
   itself a draft, and the database entry becomes a public API the moment it
   merges. Renaming `bootPath` afterwards breaks every Cocoon user.
3. Then open the PR, linking the release and this contract doc.

Opening it before there is an APK risks a silent close and burns first-impression
credit with the maintainers.

## Open questions

- Contents and encoding of a `.ps3folder` tag file. Needed before claiming
  folder-format support.
- Whether Cocoon's installed-player detection parses the `-n` package or does
  something else. Only inferred.
- Cocoon's `External display: false` flag and its `Swap` action on the Thor's two
  panels — undocumented, and relevant to a dual-screen handheld running a
  full-screen emulator.
- Whether Cocoon passes anything else we should honour (it sent RetroArch eight
  extras; our contract accepts three).
