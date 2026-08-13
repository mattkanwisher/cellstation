#!/bin/bash
# Build a flashable SD/eMMC image for the Raspberry Pi CM5 carrying Omarchy
# Quattro on an Arch Linux ARM base.
#
# This replaces upstream's x86 archiso pipeline. The strategy:
#   1. loopback image: 512M FAT32 firmware partition + ext4 root
#   2. bootstrap Arch Linux ARM aarch64 rootfs (tarball, then pacman inside
#      a qemu-user chroot when building on x86)
#   3. install the filtered omarchy package set (overlay/install/packages.*)
#   4. run upstream's own install stages unmodified in the chroot
#   5. drop in overlay/boot/config.txt and the rpi-cm5 hardware setup
#
# STATUS: staged skeleton — structure is real, but it has NOT been run
# against hardware yet. Steps marked TODO need first-boot validation.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
overlay="$here/../overlay"
upstream="$here/upstream"

IMG=${IMG:-$here/omarchy-cm5.img}
IMG_SIZE=${IMG_SIZE:-16G}
ALARM_TARBALL=${ALARM_TARBALL:-http://os.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz}

[[ -d $upstream ]] || { echo "run build/fetch-upstream.sh first" >&2; exit 1; }
[[ $EUID -eq 0 ]] || { echo "must run as root (losetup/mount/chroot)" >&2; exit 1; }

if [[ $(uname -m) != aarch64 ]]; then
  # Cross-building: need binfmt so we can chroot into the aarch64 rootfs.
  [[ -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ]] ||
    { echo "install qemu-user-static-binfmt for cross-builds" >&2; exit 1; }
fi

# --- 1. image + partitions ---------------------------------------------------
truncate -s "$IMG_SIZE" "$IMG"
parted -s "$IMG" mklabel msdos \
  mkpart primary fat32 1MiB 513MiB \
  mkpart primary ext4 513MiB 100% \
  set 1 boot on

loop=$(losetup --find --show --partscan "$IMG")
trap 'umount -R /mnt/omarchy-cm5 2>/dev/null || true; losetup -d "$loop"' EXIT

mkfs.vfat -n BOOT "${loop}p1"
mkfs.ext4 -L omarchy-root "${loop}p2"

mkdir -p /mnt/omarchy-cm5
mount "${loop}p2" /mnt/omarchy-cm5
mkdir -p /mnt/omarchy-cm5/boot
mount "${loop}p1" /mnt/omarchy-cm5/boot
root=/mnt/omarchy-cm5

# --- 2. base rootfs ----------------------------------------------------------
curl -fL "$ALARM_TARBALL" | bsdtar -xpf - -C "$root"

install -Dm644 "$overlay/pacman/pacman-cm5.conf" "$root/etc/pacman.conf"
cp /etc/resolv.conf "$root/etc/resolv.conf"

in_chroot() { arch-chroot "$root" "$@"; }

in_chroot pacman-key --init
in_chroot pacman-key --populate archlinuxarm
in_chroot pacman -Syu --noconfirm

# Pi kernel + firmware boot chain instead of upstream's linux + limine.
in_chroot pacman -S --noconfirm --needed \
  linux-rpi-16k raspberrypi-bootloader firmware-raspberrypi rpi-utils

# --- 3. omarchy package set --------------------------------------------------
# Upstream base list, minus known x86/ARM-broken entries, plus Pi additions.
mapfile -t packages < <(
  grep -vE '^\s*(#|$)' "$upstream/install/omarchy-base.packages" |
    grep -vxFf <(grep -vE '^\s*(#|$)' "$overlay/install/packages.skip")
  grep -vE '^\s*(#|$)' "$overlay/install/packages.add"
)
# TODO: first real run will shake out remaining unavailable-on-aarch64
# packages; move them into packages.skip rather than editing upstream's list.
in_chroot pacman -S --noconfirm --needed "${packages[@]}"

# --- 4. upstream install stages, unmodified ----------------------------------
cp -a "$upstream" "$root/opt/omarchy-src"
run_stage() {
  in_chroot env OMARCHY_INSTALL=/opt/omarchy-src/install \
    bash -c 'source /opt/omarchy-src/install/helpers/logging.sh 2>/dev/null; source "$1"' _ "$1"
}
# Hardware stage: every upstream probe (lspci/dmidecode for laptop hardware)
# no-ops on the CM5; our additions live in overlay, run afterwards.
run_stage /opt/omarchy-src/install/hardware/all.sh || true # TODO validate no-op behavior on first build
run_stage /opt/omarchy-src/install/config/all.sh
run_stage /opt/omarchy-src/install/login/all.sh

# --- 5. CM5 overlay ----------------------------------------------------------
install -Dm644 "$overlay/boot/config.txt" "$root/boot/config.txt"
in_chroot bash < "$overlay/install/hardware/rpi-cm5.sh"

genfstab -U "$root" >> "$root/etc/fstab"

echo "image ready: $IMG"
