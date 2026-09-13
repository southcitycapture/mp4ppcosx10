#!/bin/bash
# capture.sh -- headless Dolphin reference capture for Mario Party 4 (GM4E01).
#
#   capture.sh <seconds> <outdir> [extra Dolphin args...]
#
# Runs Dolphin in batch mode (-b, no GUI) against the reference disc with a
# pinned, isolated user directory, dumps one PNG per emulated frame into
# <outdir>/frames/framedump_N.png, then stops the emulator with SIGTERM.
#
# Frame dumping is ~0.4x real time on an M1 Max, so <seconds> of wall clock
# yields roughly <seconds>*24 emulated frames.
#
# To replay an input movie instead of running unattended:
#   capture.sh 300 out/run1 -m /path/to/movie.dtm
set -u
: "${MP4_ISO:=/Users/zachjack/PowerPC Project/ROMs/Mario Party 4 (USA) (Rev 1).nkit.iso}"
: "${MP4_DOLPHIN:=/Applications/Dolphin.app/Contents/MacOS/Dolphin}"
: "${MP4_USERDIR:?set MP4_USERDIR to the pinned Dolphin user directory}"

SECS="$1"; OUT="$2"; shift 2
export LC_ALL=C.UTF-8

rm -rf "$MP4_USERDIR/Dump/Frames"
mkdir -p "$MP4_USERDIR/Dump/Frames" "$OUT"

"$MP4_DOLPHIN" -u "$MP4_USERDIR" -b -e "$MP4_ISO" -v Vulkan "$@" \
  > "$OUT/dolphin.log" 2>&1 &
PID=$!
sleep "$SECS"
kill -TERM "$PID" 2>/dev/null
for _ in $(seq 1 20); do ps -p "$PID" >/dev/null 2>&1 || break; sleep 1; done
ps -p "$PID" >/dev/null 2>&1 && kill -9 "$PID"
sleep 2

rm -rf "$OUT/frames"
mv "$MP4_USERDIR/Dump/Frames" "$OUT/frames"
echo "frames: $(ls "$OUT/frames" | wc -l | tr -d ' ')  -> $OUT/frames"
