#!/usr/bin/env python3
"""M49 (PLAN.md 64): a Lite option's before/after picture for the user.

    m49_compare.py OUT.jpg "CAPTION" EXACT.ppm LITE.ppm [EXACT2.ppm LITE2.ppm]

The same lockstep frame drawn console-exact (left) and with the option
(right), downscaled to 400x300 each, a caption under them; a second pair
makes a second row.  Prints how many pixels differ (worst channel > 8
levels) per pair, the number the PLAN's table quotes."""
import sys
from PIL import Image, ImageChops, ImageDraw, ImageFont

W, H = 400, 300
FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'


def diff_count(a, b):
    d = ImageChops.difference(a.convert('RGB'), b.convert('RGB'))
    px = d.getdata()
    return sum(1 for p in px if max(p) > 8), len(px)


def main():
    out, caption, frames = sys.argv[1], sys.argv[2], sys.argv[3:]
    pairs = [(frames[i], frames[i + 1]) for i in range(0, len(frames) - 1, 2)]
    try:
        font = ImageFont.truetype(FONT, 14)
        small = ImageFont.truetype(FONT, 12)
    except OSError:
        font = small = ImageFont.load_default()
    cap_h = 22 + 18 * (1 + caption.count('\n'))
    img = Image.new('RGB', (2 * W + 12, len(pairs) * (H + 20) + cap_h + 4), (24, 24, 24))
    dr = ImageDraw.Draw(img)
    y = 4
    for a, b in pairs:
        ia, ib = Image.open(a).convert('RGB'), Image.open(b).convert('RGB')
        n, tot = diff_count(ia, ib)
        print('%s vs %s: %d of %d pixels differ (> 8 levels), %.2f%%' % (a, b, n, tot, 100.0 * n / tot))
        dr.text((4, y), 'console-exact', fill=(230, 230, 230), font=small)
        dr.text((W + 12, y), 'Lite', fill=(255, 210, 120), font=small)
        y += 16
        img.paste(ia.resize((W, H), Image.LANCZOS), (4, y))
        img.paste(ib.resize((W, H), Image.LANCZOS), (W + 8, y))
        y += H + 4
    dr.multiline_text((6, y + 4), caption, fill=(240, 240, 240), font=font, spacing=4)
    img.save(out, quality=88)


if __name__ == '__main__':
    main()
