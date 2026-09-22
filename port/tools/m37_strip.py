#!/usr/bin/env python3
"""M37: a labelled strip of the three fixed frames, for the milestone's
screenshot.  The point of the picture is that it is the *same* picture:
M37 moves batch boundaries, never a pixel.

    m37_strip.py OUT.png LABEL:DIR [LABEL:DIR ...]

Each DIR holds frame-000800.ppm / frame-003000.ppm / frame-007000.ppm as
--dumpframe writes them; one row per arm, one column per frame, with the
md5 of each frame under it.
"""
import hashlib, os, sys
from PIL import Image, ImageDraw

FRAMES = [(800, 'title'), (3000, 'character select'), (7000, 'board')]
W, H = 400, 300
PAD, LBL = 4, 34


def md5(path):
    with open(path, 'rb') as f:
        return hashlib.md5(f.read()).hexdigest()[:8]


def main():
    out = sys.argv[1]
    arms = [a.split(':', 1) for a in sys.argv[2:]]
    im = Image.new('RGB', (len(FRAMES) * (W + PAD) + PAD,
                           len(arms) * (H + LBL + PAD) + PAD), (24, 24, 28))
    d = ImageDraw.Draw(im)
    for r, (label, dirname) in enumerate(arms):
        y = PAD + r * (H + LBL + PAD)
        for c, (f, name) in enumerate(FRAMES):
            x = PAD + c * (W + PAD)
            p = os.path.join(dirname, 'frame-%05d.ppm' % f)
            if not os.path.exists(p):
                p = os.path.join(dirname, 'frame-%06d.ppm' % f)
            if os.path.exists(p):
                im.paste(Image.open(p).resize((W, H)), (x, y))
                d.text((x + 2, y + H + 2), "%s  %s  f%d  md5 %s" %
                       (label, name, f, md5(p)), fill=(220, 220, 220))
            else:
                d.text((x + 2, y + H // 2), "%s: missing" % p, fill=(220, 90, 90))
    im.save(out)
    print(out, im.size)


if __name__ == '__main__':
    main()
