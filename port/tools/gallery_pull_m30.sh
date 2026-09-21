#!/bin/sh
# M30: pull ~/gallery-m30/mNNN/frame-*.ppm from the G4 into ~/gallery-m30/ and
# write port/docs/gallery/mNNN-m30-fNNNNNN.jpg (320x240, as the sweep's frames).
set -e
cd "$(dirname "$0")/../.."
mkdir -p "$HOME/gallery-m30"
for g in "$@"; do
    mkdir -p "$HOME/gallery-m30/$g"
    scp -q "g4:gallery-m30/$g/*.ppm" "$HOME/gallery-m30/$g/" || true
    scp -q "g4:gallery-m30/$g.log" "$HOME/gallery-m30/$g.log" || true
    python3 - "$g" <<'PY'
import sys, glob, os
from PIL import Image
g = sys.argv[1]; home = os.path.expanduser('~')
for f in sorted(glob.glob(f'{home}/gallery-m30/{g}/frame-*.ppm')):
    n = int(os.path.basename(f)[6:11])
    Image.open(f).convert('RGB').resize((320, 240), Image.LANCZOS).save(f'port/docs/gallery/{g}-m30-f{n:06d}.jpg', 'JPEG', quality=72, optimize=True)
print(g, len(glob.glob(f'{home}/gallery-m30/{g}/frame-*.ppm')), 'frames')
PY
done
