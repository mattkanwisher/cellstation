# Feasibility: Omarchy Quattro on the Raspberry Pi CM5

*Analysis date: 2026-08-13, against `basecamp/omarchy` branch `quattro`
(`82ae514`, version `4.0.0.alpha`).*

## What Quattro actually is

Omarchy 4 "Quattro" is a large departure from Omarchy 2/3: the whole desktop
shell (bar, launcher, menus, notifications) was rewritten in **Quickshell**
(Qt6/QML), and the project moved from "curated dotfiles + installer" to a
**package-backed distro**: Omarchy itself ships as pacman packages from
`pkgs.omarchy.org`, with stable/rc/edge/dev channels, its own migrations and
update path (`omarchy-update`, `omarchy-channel-set`).

Reading the tree, the codebase decomposes as:

- `bin/` — ~400 bash scripts (`omarchy-*`). Architecture-neutral.
- `shell/` — the Quickshell QML desktop. Architecture-neutral (needs Qt6 +
  Quickshell built for aarch64).
- `config/`, `default/`, `themes/`, `applications/` — configs, themes,
  desktop entries. Architecture-neutral.
- `install/` — bash install stages run by the ISO builder against a
  pacstrapped chroot: `omarchy-base.packages`, `hardware/`, `config/`,
  `login/`, `user/`. This is where all architecture awareness lives.
- `migrations/`, `test/`, `docs/`, `manual/` — neutral.

**There is no compiled code in the omarchy repo itself.** The compiled
surface lives in the packages it installs.

## Where x86 is actually assumed

Surprisingly few places, and almost all of them fail soft:

1. **The package repo.** `default/pacman/pacman-*.conf` points at
   `https://pkgs.omarchy.org/stable/$arch` — only x86_64 is published.
   This is the single biggest blocker and it's infrastructure, not code.
2. **`[multilib]`** in pacman.conf — does not exist on ALARM (it's for
   32-bit x86 libs). One-line removal in *our* copy of the conf.
3. **Hardware scripts** (`install/hardware/`): nvidia, Intel PTL/IPU7/SOF,
   Apple T2, ASUS, Framework, Surface, Tuxedo… every one is gated behind an
   `lspci -nn | grep <pci-id>` or dmidecode product probe. On a CM5 none of
   them match → they all no-op. **No edits needed.**
4. **`install/hardware/vulkan.sh`** maps detected GPU vendor → vulkan
   package (`Intel`/`AMD`/`Apple`). Broadcom isn't in the map, so it no-ops;
   our added `rpi-cm5.sh` installs `vulkan-broadcom`/Mesa v3dv instead.
5. **`omarchy-other.packages`** (the ISO's offline-mirror superset): limine
   bootloader, `linux`, nvidia dkms, `lib32-*`, laptop firmware. We don't
   build an ISO, so this list simply isn't used by our image builder; our
   equivalents live in `overlay/install/packages.add`.

## Package audit (base list, aarch64)

Of the ~140 packages in `install/omarchy-base.packages`:

- **Available on ALARM/AUR for aarch64:** the overwhelming majority —
  hyprland, all the CLI tooling (fzf, eza, ripgrep, btop, lazygit, …),
  chromium, docker, libreoffice, pipewire stack, sddm, fcitx5, fonts,
  dotnet-runtime, mpv, obs-studio, nautilus, qt6.
- **Omarchy-custom (from pkgs.omarchy.org, need aarch64 rebuilds):**
  `aether`, `cliamp`, `herdr`, `omacalc`, `omacut`, `omawrite`,
  `omarchy-nvim`, `tensaku`, `ttfx`, `tobi-try`, `quickshell-git`,
  `hyprland-guiutils`, `hyprland-preview-share-picker`, plus the `omarchy`
  meta/shell packages themselves. Fonts (`ttf-ia-writer`,
  `woff2-font-awesome`) are `any`-arch and reusable as published.
  Many of the rest are bash/webapp wrappers that should be `any`-arch or
  trivial rebuilds; the real compiles are quickshell-git and the two
  hyprland helper tools.
- **Known-awkward on ARM (community-reported from the Pi 5 attempts):**
  `obsidian` and `pinta` (AUR builds flaky on ARM), `chromium`
  (works from ALARM but has been unstable; Brave was the workaround),
  `hyprlock` needed `-git`. None are load-bearing; all are droppable or
  substitutable via `overlay/install/packages.skip`.
- **Not applicable:** `qemu-user-static-binfmt` (ironically, we use the
  x86→ARM direction of this on the build host).

## The CM5-specific integration

- **SoC:** BCM2712, 4×Cortex-A76 — aarch64, plenty for Hyprland.
- **Kernel:** `linux-rpi-16k` (ALARM) or `linux-rpi` (4K pages fallback —
  some software historically chokes on 16K pages; keep both in the image
  recipe until proven).
- **Boot:** Pi firmware boot chain (`config.txt` on a FAT32 partition +
  EEPROM), *not* limine/mkinitcpio-limine. This bypasses upstream's
  bootloader assumptions entirely because those live in the ISO builder,
  not in the omarchy repo.
- **GPU:** VideoCore VII. Mesa `v3d` gives GLES 3.1; `v3dv` is Vulkan 1.3
  conformant. Hyprland (Aquamarine/GLES renderer) is community-proven on
  Pi 5, which is the same GPU. Quickshell is Qt6 → fine. Known wrinkle:
  fractional scaling produced black screens in the Pi 5 guide; pin integer
  scaling in the default monitor config.
- **Carrier-board specifics** (TensorFleet `vaio_cm5_carrier` /
  `koyomi-lvds-hat`): DSI/LVDS panel timings and any panel driver overlays
  go in `overlay/boot/config.txt` — this is where our *real* per-product
  value-add lands, and it's entirely additive.

## Prior art

- A community guide got Omarchy (v2-era, pre-Quattro) running on a
  **Raspberry Pi 5** using Manjaro ARM's dev branch + `OMARCHY_BARE=true`,
  after `sed`-ing out the installer's Arch guard. Main casualties were the
  x86-only commercial apps (Spotify/Zoom/1Password) — which **Quattro no
  longer ships in its base list**, so the port surface actually shrank.
- Arch Linux ARM packages `hyprland` for aarch64.
- Quattro's base list already includes `vulkan-asahi` handling for Apple
  ARM Macs via the Asahi ecosystem — evidence upstream is drifting toward
  multi-arch tolerance, which improves the odds of upstreaming guards.

## Difficulty verdict

**Milestone 1 — Hyprland desktop, Omarchy configs, no Omarchy packages
(the "OMARCHY_BARE" equivalent): days.** ALARM rootfs + hyprland + upstream
`config/`/`themes/` + bin scripts on PATH. This answers "does it feel like
Omarchy on the CM5."

**Milestone 2 — real Quattro: the Quickshell shell + omarchy packages +
update channel: 2–4 weeks part-time.** Gated on the aarch64 package
pipeline (`pkgs/`): stand up an `[omarchy]` repo with the same
`/stable/$arch` layout, rebuild the ~15 custom packages, host it (GitHub
releases or any static host), and point `overlay/pacman/pacman-cm5.conf`
at it. After that, `omarchy-update` works unmodified.

**Milestone 3 — reproducible flashable eMMC image for the CM5 fleet:
another week.** `build/mkimage.sh` productionized + CI (an aarch64 runner
or qemu-user chroot on x86).

**Upstream delta: target zero patches.** Everything so far fits in overlay
files. The only candidate patches are (a) an `uname -m` guard if any
install script hard-fails on ARM once actually run, and (b) a Broadcom
entry in `vulkan.sh`'s vendor map — both small enough to PR upstream
rather than carry.

## Risks

1. **ALARM freshness.** Quattro tracks recent Hyprland; if ALARM's
   `hyprland` lags what `quickshell`/`hyprland-guiutils` expect, we end up
   rebuilding Hyprland itself in our repo too. Contained, but it grows the
   package pipeline.
2. **Quattro is alpha.** 900+ commits ahead of dev and moving fast;
   pin `upstream.lock` and rebase deliberately.
3. **16K page kernel** breaks the odd binary (historically jemalloc-based
   apps). Mitigation: `linux-rpi` 4K variant is a config.txt swap away.
4. **Chromium-on-ARM stability** — Quattro leans on Chromium for webapps
   (`omarchy-launch-webapp`). Brave substitution is proven if needed.
5. **pkgs.omarchy.org coupling.** If upstream's package set churns weekly,
   chasing it costs real maintenance. Mitigation: stable channel only.
