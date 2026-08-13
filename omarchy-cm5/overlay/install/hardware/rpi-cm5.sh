# CM5 hardware setup — the Pi analog of upstream's install/hardware/* scripts.
# Runs inside the image chroot at the end of the build. Additive only: nothing
# in upstream is edited, this runs after upstream's hardware/all.sh (whose
# laptop probes all no-op on the CM5).

set -euo pipefail

# Hyprland on VideoCore VII: force the GLES renderer path and integer scaling
# (fractional scaling black-screens on v3d — seen on Pi 5).
mkdir -p /etc/skel/.config/hypr
cat > /etc/skel/.config/hypr/rpi-cm5.conf <<'EOF'
# Sourced from hyprland.conf; CM5-specific overrides.
monitor = , preferred, auto, 1
render {
    # v3d: no explicit_sync yet
    explicit_sync = 0
}
EOF

# power-profiles-daemon: "performance" profile is unsupported on the Pi;
# upstream defaults assume it exists. Pin balanced.
mkdir -p /etc/omarchy-cm5
echo balanced > /etc/omarchy-cm5/power-profile

# TODO(carrier): LVDS/DSI panel dtoverlay + timings for the TensorFleet
# carrier boards (vaio_cm5_carrier, koyomi-lvds-hat) — belongs in
# overlay/boot/config.txt once panel bring-up settles.
