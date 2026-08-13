#!/bin/bash
# Rebuild Omarchy's custom packages for aarch64 and assemble a pacman repo.
# Run on an aarch64 host (CM5/Pi5 build box) or inside a qemu-user chroot.
#
# STATUS: skeleton — blocked on locating upstream's PKGBUILD sources
# (not in the omarchy repo; see pkgs/README.md step 1).
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
out="$here/repo/stable/aarch64"
mkdir -p "$out"

[[ $(uname -m) == aarch64 ]] || { echo "run on aarch64 (or qemu chroot)" >&2; exit 1; }

PACKAGES=(
  # ordered roughly by build cost; any-arch repacks first
  omarchy-nvim omacalc omacut omawrite tobi-try
  ttfx herdr cliamp tensaku aether
  hyprland-guiutils hyprland-preview-share-picker
  quickshell-git
)

for pkg in "${PACKAGES[@]}"; do
  srcdir="$here/pkgbuilds/$pkg"
  if [[ ! -f $srcdir/PKGBUILD ]]; then
    echo "SKIP $pkg: no PKGBUILD vendored yet" >&2
    continue
  fi
  (cd "$srcdir" && makepkg --syncdeps --noconfirm --clean CARCH=aarch64)
  mv "$srcdir"/*.pkg.tar.zst "$out/"
done

repo-add "$out/omarchy.db.tar.zst" "$out"/*.pkg.tar.zst
echo "repo assembled at $out — publish this directory as stable/aarch64/"
