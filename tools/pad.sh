#!/bin/sh
# Drive a running CellStation game by broadcasting to the in-app debug pad.
#
# Unlike tools/drive.sh (which injects OS-level gamepad events and is blocked by
# the "first physical press" latch), this talks straight to EmulationActivity's
# DEBUG_PAD receiver, which writes through PadState.setVirtual into the native
# pad snapshot. It still needs the latch cleared once per boot by a single
# physical button press on the device; after that this drives everything.
#
# Debug builds only (the receiver is registered only when the app is debuggable).
#
#   tools/pad.sh start x x x           # press START, then Cross x3 (3s apart)
#   tools/pad.sh -g 1.5 down cross     # 1.5s between presses
#   tools/pad.sh -h 400 start          # hold each press 400ms
#   tools/pad.sh arcade                # macro: menu -> Arcade -> Solo -> fight
#
# Buttons: start select cross/x circle/o square triangle up down left right
#          l1 r1 l2 r2 l3 r3 ps
#
# ANDROID_SERIAL selects the device (else the only attached one).

set -e

PKG=nu.hyperworks.cellstation
ACTION=$PKG.DEBUG_PAD
HOLD=250
GAP=3000
ADB="adb${ANDROID_SERIAL:+ -s $ANDROID_SERIAL}"

while [ $# -gt 0 ]; do
	case "$1" in
	-h) HOLD="$2"; shift 2 ;;
	-g) GAP=$(awk "BEGIN{print int($2*1000)}"); shift 2 ;;
	-*) echo "unknown option: $1" >&2; exit 2 ;;
	*) break ;;
	esac
done

[ $# -gt 0 ] || { sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

# Macro: from the main menu (post-latch) into an Arcade solo fight.
if [ "$1" = "arcade" ]; then
	set -- cross cross down cross cross cross cross cross
fi

# Normalise aliases and join with commas for a single broadcast (the receiver
# walks the list with its own hold/gap timing, so one call presses the sequence).
keys=""
for b in "$@"; do
	case "$b" in
		x) b=cross ;; o) b=circle ;;
	esac
	keys="${keys:+$keys,}$b"
done

echo "pad: $keys  (hold ${HOLD}ms, gap ${GAP}ms)"
$ADB shell am broadcast -a "$ACTION" --es keys "$keys" --ei hold "$HOLD" --ei gap "$GAP" >/dev/null
