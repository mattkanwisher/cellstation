# aarch64 package pipeline

The single biggest piece of this port. Upstream serves its custom packages
from `https://pkgs.omarchy.org/stable/$arch` — x86_64 only. We host a repo
with the **same path shape** for aarch64 so that upstream's pacman configs,
channel switching (`omarchy-channel-set`), and `omarchy-update` all work
without modification — only the `Server =` line differs
(see `overlay/pacman/pacman-cm5.conf`).

## Packages to provide (audited against quattro `82ae514` base list)

| Package | Kind | Expected effort |
|---|---|---|
| `omarchy` (+ shell/meta pkgs) | bash + QML | `any`-arch repack, trivial |
| `omarchy-nvim` | config | `any`-arch, trivial |
| `omacalc`, `omacut`, `omawrite` | app wrappers | trivial |
| `tobi-try`, `ttfx`, `herdr`, `cliamp`, `tensaku`, `aether` | mixed CLI tools | rebuild; effort depends on language/runtime |
| `quickshell-git` | Qt6/C++ | real compile; the critical one |
| `hyprland-guiutils` | C++ | compile |
| `hyprland-preview-share-picker` | C++ | compile |
| `ttf-ia-writer`, `woff2-font-awesome` | fonts | `any`-arch — reuse upstream's artifacts as-is |
| `hyprland` (contingency) | C++ | only if ALARM's build lags what quickshell expects |

## Build strategy

1. Obtain PKGBUILDs (upstream packaging repo, or reconstruct from the
   published x86_64 packages' .PKGINFO — TODO: locate upstream's packaging
   source; the omarchy repo itself does not contain PKGBUILDs).
2. Build natively on a CM5/Pi5 build box (cleanest), or in a qemu-user
   aarch64 chroot on x86 CI (slower; fine for the small package count).
3. `repo-add omarchy.db.tar.zst *.pkg.tar.zst`, publish the directory as
   `stable/aarch64/` on any static host (GitHub releases works).

`rebuild-aarch64.sh` sketches the loop.
