#!/bin/bash
# pick.sh <framedir> <destdir> <prefix> <first> <last> <step>
# Copies every <step>-th full-size frame down to 320x264 (exact half of
# Dolphin's 1x native 640x528) as <destdir>/<prefix>-NNNN.png, numbered by the
# ORIGINAL emulated frame number so the name is the comparison key.
set -eu
D="$1"; DEST="$2"; PRE="$3"; A="$4"; B="$5"; STEP="$6"
mkdir -p "$DEST"
for n in $(seq "$A" "$STEP" "$B"); do
  f="$D/framedump_$n.png"
  [ -f "$f" ] || continue
  magick "$f" -resize 320x264! -strip "$DEST/$PRE-$(printf '%04d' "$n").png"
done
echo "wrote $(ls "$DEST" | grep -c "^$PRE-") frames to $DEST"
