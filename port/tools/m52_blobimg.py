#!/usr/bin/env python3
"""M52 (PLAN.md 67): a round-shadow option's picture and its visual cost.

    m52_blobimg.py CONSOLE_DIR BLOB_DIR OUT.jpg --title "m401 Manta Rings" [--frames A,B]

CONSOLE_DIR and BLOB_DIR hold the same lockstep frames (frame-NNNNN.ppm:
--mgdump of a --nolite run and of a --liteopts mNNN.blob run).  The picture:
for each frame, the console's and the round shadows' side by side (each
downscaled to 400 wide), and under them the patch where the two differ most,
enlarged x2 -- the shadows themselves.  The visual cost printed (and used by
Benchmark Mode's order, cheapest first): the mean absolute difference of the
two pictures over all pixels and channels, 0-255, averaged over the frames."""
import argparse, glob, os, sys
from PIL import Image, ImageChops, ImageDraw, ImageStat


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a'); ap.add_argument('b'); ap.add_argument('out')
    ap.add_argument('--title', default='')
    ap.add_argument('--frames', default='')
    ap.add_argument('--opt', default='')
    o = ap.parse_args()
    fa = sorted(glob.glob(os.path.join(o.a, 'frame-*.ppm')))
    names = [os.path.basename(p) for p in fa if os.path.exists(os.path.join(o.b, os.path.basename(p)))]
    if o.frames:
        want = set('frame-%05d.ppm' % int(x) for x in o.frames.split(','))
        names = [n for n in names if n in want]
    if not names:
        sys.exit('no common frames')
    rows, costs = [], []
    for n in names:
        A = Image.open(os.path.join(o.a, n)).convert('RGB')
        B = Image.open(os.path.join(o.b, n)).convert('RGB')
        d = ImageChops.difference(A, B)
        costs.append(sum(ImageStat.Stat(d).mean) / 3.0)
        # the most changed 160x120 patch (a coarse search on the grey difference)
        g = d.convert('L')
        W, H = g.size
        best, bx, by = -1, 0, 0
        for y in range(0, H - 120 + 1, 20):
            for x in range(0, W - 160 + 1, 20):
                s = ImageStat.Stat(g.crop((x, y, x + 160, y + 120))).sum[0]
                if s > best:
                    best, bx, by = s, x, y
        box = (bx, by, bx + 160, by + 120)
        sa = A.resize((400, 300), Image.LANCZOS)
        sb = B.resize((400, 300), Image.LANCZOS)
        ca = A.crop(box).resize((320, 240), Image.NEAREST)
        cb = B.crop(box).resize((320, 240), Image.NEAREST)
        row = Image.new('RGB', (820, 300 + 250 + 24), (24, 24, 24))
        row.paste(sa, (0, 24)); row.paste(sb, (420, 24))
        row.paste(ca, (40, 24 + 305)); row.paste(cb, (460, 24 + 305))
        dr = ImageDraw.Draw(row)
        fr = n[6:11].lstrip('0')
        dr.text((4, 6), 'console (Lite off)  frame %s' % fr, fill=(230, 230, 230))
        dr.text((424, 6), 'round shadows (%s)' % (o.opt or o.title.split()[0] + '.blob'), fill=(230, 230, 230))
        dr.rectangle([sa.size[0] * bx // W, 24 + 300 * by // H, sa.size[0] * (bx + 160) // W, 24 + 300 * (by + 120) // H], outline=(255, 220, 0))
        dr.rectangle([420 + 400 * bx // W, 24 + 300 * by // H, 420 + 400 * (bx + 160) // W, 24 + 300 * (by + 120) // H], outline=(255, 220, 0))
        rows.append(row)
    H = sum(r.size[1] for r in rows) + 30
    out = Image.new('RGB', (820, H), (24, 24, 24))
    ImageDraw.Draw(out).text((4, 8), '%s -- the console\'s projected shadows / the round shadows (M52), the same lockstep frames; the boxed patch x2 below' % o.title, fill=(255, 255, 255))
    y = 30
    for r in rows:
        out.paste(r, (0, y)); y += r.size[1]
    out.save(o.out, quality=85)
    print('%s visual cost %.2f (mean abs difference, 0-255) over %s' % (o.title, sum(costs) / len(costs), ', '.join(n[6:11] for n in names)))


if __name__ == '__main__':
    main()
