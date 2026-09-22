#!/usr/bin/env python3
"""M38 (PLAN.md 53.9): find the Dolphin frame a port movie frame shows, and
say how far apart the two pictures are.

Linux Dolphin's frame dump is an FFV1 AVI whose muxer drops frames
(reference-dolphin.md 4), so its frame numbers are not retraces.  A movie
frame is found by its content instead: every port --dumpframe PPM is scaled
into the same 160x120 grey space as the capture (Dolphin's 640x528 and the
port's 640x480 both scaled whole), and the capture is searched
for the frame with the least mean difference.  Dolphin's 640x528 is the
whole 480-line frame scaled by 1.1 (the port's black bars above and below the
movie are in it), so it is scaled, not cropped.  The winner's full-size frame
is then cut out of the AVI and compared in colour, on the movie's own
rectangle, at the capture's geometry.

  m38_match.py GRAY.raw AVI OUTDIR PORT.ppm [PORT.ppm ...]

GRAY.raw is `ffmpeg -i AVI -vf scale=160:120,format=gray
-f rawvideo`.  Prints one line a frame: the match, its grey distance, and the
colour comparison (mean per channel, mean |diff|, share of pixels within 2
and 8 levels) over the movie rectangle.
"""
import os
import subprocess
import sys

from PIL import Image

W, H = 160, 120
FFMPEG = os.path.expanduser('~/bin/ffmpeg')


def gray_of_port(path):
    im = Image.open(path).convert('L').resize((W, H), Image.BILINEAR)
    return im.tobytes()


def main():
    raw, avi, out = sys.argv[1:4]
    ports = sys.argv[4:]
    data = open(raw, 'rb').read()
    n = len(data) // (W * H)
    frames = [data[i * W * H:(i + 1) * W * H] for i in range(n)]
    # subsample for the search: every 3rd pixel of every 2nd row
    idx = [y * W + x for y in range(8, H - 8, 2) for x in range(8, W - 8, 3)]
    fsub = [bytes(f[i] for i in idx) for f in frames]
    os.makedirs(out, exist_ok=True)
    for p in ports:
        g = gray_of_port(p)
        # the search window: a --dumpframe N is near capture frame N (the
        # capture starts at the same boot, and drops only a few frames)
        gs = bytes(g[i] for i in idx)
        best, bi = 1e9, -1
        for k in range(n):
            d = sum(abs(a - b) for a, b in zip(gs, fsub[k])) / len(gs)
            if d < best:
                best, bi = d, k
        # the matched frame, full size, from the AVI (0-based frame index)
        dpath = os.path.join(out, 'dolphin-%05d.png' % bi)
        if not os.path.exists(dpath):
            subprocess.run([FFMPEG, '-v', 'error', '-y', '-i', avi, '-vf',
                            'select=eq(n\\,%d)' % bi, '-vframes', '1', dpath], check=True)
        d_im = Image.open(dpath).convert('RGB').resize((640, 480), Image.BILINEAR)
        p_im = Image.open(p).convert('RGB')
        # the movie's rectangle: 608x448 centred on (288+32, 240) of the
        # 640-wide frame -- the sprite at 288,240 in the game's 576-wide space
        box = (8, 16 + 8, 632, 464 - 8)
        a = p_im.crop(box).tobytes()
        b = d_im.crop(box).tobytes()
        sums = [0, 0, 0]
        absd = 0
        w2 = w8 = 0
        npx = len(a) // 3
        for i in range(0, len(a), 3):
            m = 0
            for c in range(3):
                d = a[i + c] - b[i + c]
                sums[c] += d
                ad = d if d >= 0 else -d
                absd += ad
                if ad > m:
                    m = ad
            if m <= 2:
                w2 += 1
            if m <= 8:
                w8 += 1
        print('%s -> dolphin #%d (grey %.2f): mean R %+.2f G %+.2f B %+.2f, |d| %.2f, '
              '<=2: %.1f%%, <=8: %.1f%%' % (
                  os.path.basename(p), bi, best, sums[0] / npx, sums[1] / npx, sums[2] / npx,
                  absd / (3 * npx), 100.0 * w2 / npx, 100.0 * w8 / npx))
        pair = Image.new('RGB', (640 * 2, 480))
        pair.paste(p_im, (0, 0))
        pair.paste(d_im, (640, 0))
        pair.save(os.path.join(out, 'pair-' + os.path.basename(p).replace('.ppm', '.png')))


if __name__ == '__main__':
    main()
