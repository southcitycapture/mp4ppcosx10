#!/bin/sh
# M39 (from M35's): pull ~/gallery-m39/mNNN/frame-*.ppm from the G4 into ~/gallery-m39/ and
# write port/docs/gallery/mNNN-m39-fNNNNNN.jpg (320x240, as the sweep's frames).
# G4DIR=gallery-m39f pulls the final build's whole-gallery run instead (the same tag: the
# final build's rows replace the 29 of the first run).
set -e
cd "$(dirname "$0")/../.."
mkdir -p "$HOME/gallery-m39"
for g in "$@"; do
    mkdir -p "$HOME/gallery-m39/$g"
    scp -q "g4:${G4DIR:-gallery-m39}/$g/*.ppm" "$HOME/gallery-m39/$g/" || true
    scp -q "g4:${G4DIR:-gallery-m39}/$g.log" "$HOME/gallery-m39/$g.log" || true
    python3 - "$g" <<'PY'
import sys, glob, os
from PIL import Image
g = sys.argv[1]; home = os.path.expanduser('~')
for f in sorted(glob.glob(f'{home}/gallery-m39/{g}/frame-*.ppm')):
    n = int(os.path.basename(f)[6:11])
    Image.open(f).convert('RGB').resize((320, 240), Image.LANCZOS).save(f'port/docs/gallery/{g}-m39-f{n:06d}.jpg', 'JPEG', quality=72, optimize=True)
print(g, len(glob.glob(f'{home}/gallery-m39/{g}/frame-*.ppm')), 'frames')
PY
done
