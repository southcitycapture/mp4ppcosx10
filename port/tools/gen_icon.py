#!/usr/bin/env python3
"""M32: the application icon (port/resources/MarioParty4.icns), drawn here so
no artwork of the game's is shipped: a die face on a rounded square, in the
classic icns entries Leopard reads (it32/t8mk 128, il32/l8mk 32, is32/s8mk 16;
the RGB planes PackBits-style RLE'd, the masks raw).  Needs Pillow.

    python3 port/tools/gen_icon.py
"""
import os, struct
from PIL import Image, ImageDraw

here = os.path.dirname(os.path.abspath(__file__))
out = os.path.join(here, '..', 'resources', 'MarioParty4.icns')


def render(n):
    S = n * 4
    im = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    m = int(S * 0.06)
    r = int(S * 0.18)
    d.rounded_rectangle((m, m, S - m, S - m), radius=r, fill=(72, 52, 160, 255))
    d.rounded_rectangle((m + S * 0.03, m + S * 0.03, S - m - S * 0.03, S * 0.5),
                        radius=int(r * 0.8), fill=(98, 80, 190, 255))
    d.rounded_rectangle((m, m, S - m, S - m), radius=r, outline=(40, 28, 100, 255),
                        width=max(1, S // 64))
    pr = int(S * 0.085)
    for cx, cy in [(0.3, 0.3), (0.7, 0.3), (0.5, 0.5), (0.3, 0.7), (0.7, 0.7)]:
        x, y = cx * S, cy * S
        d.ellipse((x - pr, y - pr, x + pr, y + pr), fill=(255, 255, 255, 255))
        d.ellipse((x - pr * 0.55, y - pr * 0.55, x + pr * 0.15, y + pr * 0.15),
                  fill=(255, 230, 120, 255))
    return im.resize((n, n), Image.LANCZOS)


def rle(chan):
    out = bytearray()
    i = 0
    L = len(chan)
    while i < L:
        j = i
        while j + 1 < L and chan[j + 1] == chan[i] and j - i < 129:
            j += 1
        run = j - i + 1
        if run >= 3:
            out += bytes([0x80 + run - 3, chan[i]])
            i += run
            continue
        k = i
        while k < L and k - i < 128:
            if k + 2 < L and chan[k] == chan[k + 1] == chan[k + 2]:
                break
            k += 1
        out += bytes([k - i - 1]) + bytes(chan[i:k])
        i = k
    return bytes(out)


def chunk(tag, data):
    return tag.encode() + struct.pack('>I', 8 + len(data)) + data


body = b''
for n, ctag, mtag in [(128, 'it32', 't8mk'), (32, 'il32', 'l8mk'), (16, 'is32', 's8mk')]:
    px = render(n).tobytes()
    data = (b'\0\0\0\0' if ctag == 'it32' else b'')
    data += rle(px[0::4]) + rle(px[1::4]) + rle(px[2::4])
    body += chunk(ctag, data) + chunk(mtag, px[3::4])
icns = b'icns' + struct.pack('>I', 8 + len(body)) + body
open(out, 'wb').write(icns)
print('%s: %d bytes' % (out, len(icns)))
