#!/bin/sh
# M34 (from M31's): pull ~/gallery-m34/mNNN/frame-*.ppm from the G4 into ~/gallery-m34/ and
# write port/docs/gallery/mNNN-m34-fNNNNNN.jpg (320x240, as the sweep's frames).
set -e
cd "$(dirname "$0")/../.."
mkdir -p "$HOME/gallery-m34"
for g in "$@"; do
    mkdir -p "$HOME/gallery-m34/$g"
    scp -q "g4:gallery-m34/$g/*.ppm" "$HOME/gallery-m34/$g/" || true
    scp -q "g4:gallery-m34/$g.log" "$HOME/gallery-m34/$g.log" || true
    python3 - "$g" <<'PY'
import sys, glob, os
from PIL import Image
g = sys.argv[1]; home = os.path.expanduser('~')
for f in sorted(glob.glob(f'{home}/gallery-m34/{g}/frame-*.ppm')):
    n = int(os.path.basename(f)[6:11])
    Image.open(f).convert('RGB').resize((320, 240), Image.LANCZOS).save(f'port/docs/gallery/{g}-m34-f{n:06d}.jpg', 'JPEG', quality=72, optimize=True)
print(g, len(glob.glob(f'{home}/gallery-m34/{g}/frame-*.ppm')), 'frames')
PY
done
