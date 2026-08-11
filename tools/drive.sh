#!/bin/sh
# Drive a running game over adb by injecting gamepad events.
#
# Menu navigation is most of the cost of any on-device measurement: getting to
# a comparable scene by hand takes minutes and is not repeatable. This presses
# buttons with a gap long enough for a scene transition to finish.
#
#   tools/drive.sh start x x x        # START, then Cross three times
#   tools/drive.sh -d 10 start x      # 10s between presses instead of 6
#   tools/drive.sh -s start x         # screenshot after every press
#
# Buttons: start select cross/x circle/o square triangle up down left right
#          l1 r1 l2 r2 l3 r3 ps  (and 'wait' to just pause a beat)
#
# KNOWN LIMITATION: injection does not work until a *physical* button has been
# pressed at least once since the game booted. Before that the events reach the
# app (PadState.onKey consumes them, the touch overlay retires) but the game
# does not act on them. Cause not yet understood — see docs/BENCHMARKS.md. So
# press one button on the real pad, then this works for the rest of the session.

set -e

DELAY=6
SHOT=0
SHOTDIR="${TMPDIR:-/tmp}/cellstation-drive"

while [ $# -gt 0 ]; do
	case "$1" in
	-d) DELAY="$2"; shift 2 ;;
	-s) SHOT=1; shift ;;
	-*) echo "unknown option: $1" >&2; exit 2 ;;
	*) break ;;
	esac
done

[ $# -gt 0 ] || { sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

# The controller node is not a fixed number across devices or reboots.
DEV=$(adb shell 'grep -l . /sys/class/input/event*/device/name 2>/dev/null | while read f; do
        n=$(cat "$f"); case "$n" in *Controller*|*controller*|*Gamepad*|*gamepad*)
          echo "/dev/input/$(basename $(dirname $(dirname $f)))";; esac; done' | tr -d '\r' | head -1)

if [ -z "$DEV" ]; then
	echo "no gamepad input device found; is the controller attached?" >&2
	exit 1
fi

echo "using $DEV, ${DELAY}s between presses"

code_for() {
	case "$1" in
	cross|x|a) echo 304 ;;      # BTN_SOUTH
	circle|o|b) echo 305 ;;     # BTN_EAST
	square) echo 307 ;;         # BTN_NORTH  (labels differ; positions do not)
	triangle) echo 308 ;;       # BTN_WEST
	l1) echo 310 ;; r1) echo 311 ;;
	l2) echo 312 ;; r2) echo 313 ;;
	select) echo 314 ;; start) echo 315 ;; ps|mode) echo 316 ;;
	l3) echo 317 ;; r3) echo 318 ;;
	up) echo 544 ;; down) echo 545 ;; left) echo 546 ;; right) echo 547 ;;
	*) echo "" ;;
	esac
}

[ "$SHOT" = 1 ] && mkdir -p "$SHOTDIR"
n=0

for btn in "$@"; do
	n=$((n + 1))

	if [ "$btn" = "wait" ]; then
		echo "  [$n] wait ${DELAY}s"
		sleep "$DELAY"
		continue
	fi

	code=$(code_for "$btn")
	if [ -z "$code" ]; then
		echo "unknown button: $btn" >&2
		exit 2
	fi

	# Press and release as one shell invocation with a real hold in between:
	# separate `sendevent` processes release within a millisecond, which the
	# pad poll can miss entirely.
	adb shell "D=$DEV; sendevent \$D 1 $code 1; sendevent \$D 0 0 0; sleep 0.25; sendevent \$D 1 $code 0; sendevent \$D 0 0 0"
	echo "  [$n] $btn"
	sleep "$DELAY"

	if [ "$SHOT" = 1 ]; then
		out="$SHOTDIR/$(printf '%02d' $n)-$btn.png"
		adb exec-out screencap -p > "$out"
		echo "       -> $out"
	fi
done
