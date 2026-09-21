#!/bin/sh
# M31 (from M30's): pull ~/gallery-m31/mNNN/frame-*.ppm from the G4 into ~/gallery-m31/ and
# write port/docs/gallery/mNNN-m31-fNNNNNN.jpg (320x240, as the sweep's frames).
set -e
cd "$(dirname "$0")/../.."
mkdir -p "$HOME/gallery-m31"
for g in "$@"; do
    mkdir -p "$HOME/gallery-m31/$g"
    scp -q "g4:gallery-m31/$g/*.ppm" "$HOME/gallery-m31/$g/" || true
    scp -q "g4:gallery-m31/$g.log" "$HOME/gallery-m31/$g.log" || true
    python3 - "$g" <<'PY'
import sys, glob, os
from PIL import Image
g = sys.argv[1]; home = os.path.expanduser('~')
for f in sorted(glob.glob(f'{home}/gallery-m31/{g}/frame-*.ppm')):
    n = int(os.path.basename(f)[6:11])
    Image.open(f).convert('RGB').resize((320, 240), Image.LANCZOS).save(f'port/docs/gallery/{g}-m31-f{n:06d}.jpg', 'JPEG', quality=72, optimize=True)
print(g, len(glob.glob(f'{home}/gallery-m31/{g}/frame-*.ppm')), 'frames')
PY
done
