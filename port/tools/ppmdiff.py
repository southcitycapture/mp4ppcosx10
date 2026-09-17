#!/usr/bin/env python3
"""Compare two --dumpframe PPMs, and say how they differ rather than whether.

M11 moves phase 2 of the vertex path onto the GPU, and the three reference
md5s of PLAN.md §21.1 move with it -- a vertex unit is not a 7450, and the CPU
path quantised the lit colour to eight bits before the rasteriser ever saw it.
"the md5 changed" is not a finding; this is what turns it into one.

  port/tools/ppmdiff.py A.ppm B.ppm [--crop X,Y,W,H --scale N --out diff.png]

Prints the histogram of per-channel absolute differences, the worst pixel, and
-- the number that settles a rounding argument -- how many pixels differ by
more than one, two and eight levels.  With --out it writes a magnified
side-by-side of A, B and a 16x-amplified difference over the crop.
"""
import sys


def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    if not data.startswith(b'P6'):
        raise SystemExit('%s: not a P6 PPM' % path)
    # header: P6 <w> <h> <max>, whitespace/comment separated
    fields, i = [], 2
    while len(fields) < 3:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b'#':
            while data[i:i + 1] not in (b'\n', b''):
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    i += 1
    w, h, _mx = fields
    return w, h, data[i:i + w * h * 3]


def main(argv):
    if len(argv) < 3:
        raise SystemExit(__doc__)
    a_path, b_path = argv[1], argv[2]
    crop = None
    out = None
    scale = 8
    k = 3
    while k < len(argv):
        if argv[k] == '--crop':
            crop = tuple(int(v) for v in argv[k + 1].split(','))
            k += 2
        elif argv[k] == '--scale':
            scale = int(argv[k + 1])
            k += 2
        elif argv[k] == '--out':
            out = argv[k + 1]
            k += 2
        else:
            raise SystemExit('unknown option %s' % argv[k])

    wa, ha, pa = read_ppm(a_path)
    wb, hb, pb = read_ppm(b_path)
    if (wa, ha) != (wb, hb):
        raise SystemExit('different sizes: %dx%d vs %dx%d' % (wa, ha, wb, hb))
    n = wa * ha * 3

    hist = [0] * 256
    worst = 0
    worst_at = 0
    for i in range(n):
        d = pa[i] - pb[i]
        if d < 0:
            d = -d
        hist[d] += 1
        if d > worst:
            worst = d
            worst_at = i
    diff_px = 0
    for y in range(ha):
        row = y * wa * 3
        for x in range(wa):
            o = row + x * 3
            if pa[o] != pb[o] or pa[o + 1] != pb[o + 1] or pa[o + 2] != pb[o + 2]:
                diff_px += 1

    print('%s  vs  %s   %dx%d' % (a_path, b_path, wa, ha))
    print('pixels differing at all : %d of %d (%.3f%%)'
          % (diff_px, wa * ha, 100.0 * diff_px / (wa * ha)))
    for thr in (1, 2, 4, 8, 16, 32):
        over = sum(hist[thr + 1:])
        print('channel samples differing by more than %-2d : %9d (%.4f%%)'
              % (thr, over, 100.0 * over / n))
    px = worst_at // 3
    print('worst channel difference: %d at pixel (%d,%d)'
          % (worst, px % wa, px // wa))
    mean = sum(d * hist[d] for d in range(256)) / float(n)
    print('mean absolute channel difference: %.5f levels' % mean)

    if out:
        if crop is None:
            crop = (0, 0, wa, ha)
        cx, cy, cw, ch = crop
        rows = []
        for y in range(ch):
            row = bytearray()
            for panel in range(3):
                for x in range(cw):
                    o = ((cy + y) * wa + (cx + x)) * 3
                    if panel == 0:
                        px3 = pa[o:o + 3]
                    elif panel == 1:
                        px3 = pb[o:o + 3]
                    else:
                        px3 = bytes(min(255, abs(pa[o + c] - pb[o + c]) * 16)
                                    for c in range(3))
                    row += px3 * scale
            for _ in range(scale):
                rows.append(bytes(row))
        with open(out, 'wb') as f:
            f.write(b'P6\n%d %d\n255\n' % (cw * 3 * scale, ch * scale))
            for r in rows:
                f.write(r)
        print('wrote %s (A | B | 16x difference, crop %s, scale %d)'
              % (out, crop, scale))


if __name__ == '__main__':
    main(sys.argv)
