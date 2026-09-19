#!/bin/bash
# capture-linux.sh -- headless Dolphin reference capture for Mario Party 4 on a
# Linux host (littlejelly: Ivy Bridge HD 4000, Flatpak Dolphin, OpenGL backend
# -- Ivy Bridge Vulkan is not reliable in Mesa).  Same contract as capture.sh:
#
#   capture-linux.sh <seconds> <outdir> [extra Dolphin args...]
#
# Env: MP4_ISO (default ~/MarioParty4/mp4.nkit.iso), MP4_USERDIR (required;
# the pinned user dir, seeded from port/ref/dolphin-user), MP4_DOLPHIN
# (default: the Flatpak).  Frames land in $MP4_USERDIR/Dump/Frames.
# Flatpak needs the ISO and the user dir under $HOME (it can see the home
# directory by default); --filesystem= flags cover anything else.
set -u
: "${MP4_ISO:=$HOME/MarioParty4/mp4.nkit.iso}"
: "${MP4_USERDIR:?set MP4_USERDIR to the pinned Dolphin user directory}"
: "${MP4_DOLPHIN:=flatpak run --filesystem=home org.DolphinEmu.dolphin-emu}"
SECS="$1"; OUT="$2"; shift 2
export LC_ALL=C.UTF-8
rm -rf "$MP4_USERDIR/Dump/Frames"
mkdir -p "$MP4_USERDIR/Dump/Frames" "$OUT"
$MP4_DOLPHIN -u "$MP4_USERDIR" -b -e "$MP4_ISO" -v OGL "$@" \
  > "$OUT/dolphin.log" 2>&1 &
PID=$!
sleep "$SECS"
kill -TERM "$PID" 2>/dev/null
for _ in $(seq 1 20); do ps -p "$PID" >/dev/null 2>&1 || break; sleep 1; done
ps -p "$PID" >/dev/null 2>&1 && kill -9 "$PID"
sleep 2
echo "frames: $(ls "$MP4_USERDIR/Dump/Frames" 2>/dev/null | wc -l)"
