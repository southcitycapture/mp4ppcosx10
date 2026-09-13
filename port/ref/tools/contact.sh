#!/bin/bash
# contact.sh <framedir> <first> <last> <step> <out.png> [tilecols]
# Builds a labelled contact sheet so a long capture can be scanned quickly.
set -eu
D="$1"; A="$2"; B="$3"; STEP="$4"; OUT="$5"; COLS="${6:-6}"
TMP=$(mktemp -d)
for n in $(seq "$A" "$STEP" "$B"); do
  f="$D/framedump_$n.png"
  [ -f "$f" ] || continue
  magick "$f" -resize 240x -bordercolor '#202020' -border 2 \
    -background '#202020' -fill white -pointsize 16 label:"$n" -gravity center -append \
    "$TMP/$(printf '%07d' "$n").png"
done
montage "$TMP"/*.png -tile "${COLS}x" -geometry +2+2 -background '#101010' "$OUT"
rm -rf "$TMP"
echo "$OUT"
