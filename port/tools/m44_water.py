#!/usr/bin/env python3
"""M44 (PLAN.md 59): the water beside the console.

    python3 port/tools/m44_water.py PORTDIR OUTDIR

PORTDIR holds the chain's W: runs pulled from the G4 (W-GAME-ARM/frame-N.ppm,
W-GAME-ARM.log).  For m417, m405 and m434: the port's frames at the module's
entry +400 / +1,200 / +2,300 at each level (off, cheap, full) against the
console's at the same events -- Dolphin's M35 oracle capture on littlejelly
(~/mp4-sweep-work/frames/GAMEm35, the frames kept around each event; the
console's XFB is 640x528 and is scaled to the EFB's 640x480 first).  Prints,
per game, position and level: sim = 100*(1 - mean|d|/255) over RGB and >8 =
% of pixels whose worst channel differs by more than 8 levels, over the whole
frame and over the water's box; writes OUTDIR/m44-water-GAME.jpg (2x2: off,
cheap / full, console, each 400x300, at the position named in POS) and
OUTDIR/m44-water.tsv.  The two runs do not share their RNG (PLAN.md 41b), so
the characters are elsewhere; the water's box is where to look."""
import glob
import os
import re
import sys

from PIL import Image, ImageChops, ImageDraw, ImageStat

HOME = os.path.expanduser("~")
CONSOLE = {  # the M35 capture's dump index at each event (the kept frames' centres)
    "m417": {"e400": 12694, "e1200": 13494, "e2300": 14594},
    "m405": {"e400": 10937, "e1200": 11737, "e2300": 12837},
    "m434": {"e400": 11159, "e1200": 11959, "e2300": 13059},
}
EV = {"e400": 400, "e1200": 1200, "e2300": 2300}
BOX = {  # the water, in 640x480 EFB pixels (x, y, w, h)
    "m417": (60, 110, 520, 330),
    "m405": (0, 250, 640, 230),
    "m434": (160, 150, 330, 190),
}
POS = {"m417": "e1200", "m405": "e1200", "m434": "e1200"}
LEVELS = ["off", "cheap", "full"]


def stats(a, b, box=None):
    if box:
        x, y, w, h = box
        a = a.crop((x, y, x + w, y + h))
        b = b.crop((x, y, x + w, y + h))
    d = ImageChops.difference(a, b)
    mean = sum(ImageStat.Stat(d).mean) / 3.0
    px = d.getdata()
    over = sum(1 for p in px if max(p) > 8)
    return 100.0 * (1.0 - mean / 255.0), 100.0 * over / max(1, len(px))


def entry_of(log):
    for line in open(log, errors="replace"):
        m = re.search(r"entered minigame \S+ \(mg \d+\) at frame (\d+)", line)
        if m:
            return int(m.group(1))
    return None


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    tsv = open(os.path.join(out, "m44-water.tsv"), "w")
    tsv.write("game\tposition\tlevel\tsim\t>8\tsim water\t>8 water\n")
    for g in CONSOLE:
        frames = {}
        for lv in LEVELS:
            d = glob.glob(os.path.join(src, "W-%s-*--water,%s" % (g, lv)))
            if not d:
                continue
            e = entry_of(d[0] + ".log")
            if e is None:
                continue
            for k, off in EV.items():
                f = os.path.join(d[0], "frame-%05d.ppm" % (e + off))
                if os.path.exists(f):
                    frames[(lv, k)] = Image.open(f).convert("RGB")
        cons = {}
        for k, n in CONSOLE[g].items():
            f = os.path.join(HOME, "mp4-sweep-work/frames/%sm35/f%06d.png" % (g, n))
            if os.path.exists(f):
                cons[k] = Image.open(f).convert("RGB").resize((640, 480), Image.BILINEAR)
        for k in EV:
            if k not in cons:
                continue
            for lv in LEVELS:
                if (lv, k) not in frames:
                    continue
                s1, o1 = stats(frames[(lv, k)], cons[k])
                s2, o2 = stats(frames[(lv, k)], cons[k], BOX[g])
                print("%s %-5s %-5s  sim %.1f  >8 %.1f%%   water: sim %.1f  >8 %.1f%%"
                      % (g, k, lv, s1, o1, s2, o2))
                tsv.write("%s\t%s\t%s\t%.1f\t%.1f\t%.1f\t%.1f\n" % (g, k, lv, s1, o1, s2, o2))
        k = POS[g]
        if k in cons and all((lv, k) in frames for lv in LEVELS):
            W, H = 400, 300
            sheet = Image.new("RGB", (2 * W, 2 * H + 40), (0, 0, 0))
            panels = [(frames[("off", k)], "port --water off"), (frames[("cheap", k)], "port --water cheap"),
                      (frames[("full", k)], "port --water full"), (cons[k], "console (Dolphin)")]
            dr = ImageDraw.Draw(sheet)
            for i, (im, label) in enumerate(panels):
                x, y = (i % 2) * W, (i // 2) * (H + 20)
                sheet.paste(im.resize((W, H), Image.LANCZOS), (x, y + 20))
                dr.text((x + 6, y + 4), "%s  %s entry+%d" % (label, g, EV[k]), fill=(255, 255, 255))
            sheet.save(os.path.join(out, "m44-water-%s.jpg" % g), quality=85)
            print("wrote %s" % os.path.join(out, "m44-water-%s.jpg" % g))


if __name__ == "__main__":
    main()
