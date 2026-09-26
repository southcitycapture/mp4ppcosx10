#!/usr/bin/env python3
"""M45 (PLAN.md 60): the water's tuning beside the console.

    python3 port/tools/m45_water.py PORTDIR OUTDIR

PORTDIR holds tools/m45_chain.sh's W: runs pulled from the G4 (W-GAME-ARM/frame-N.ppm
and W-GAME-ARM.log).  For each of m417, m405 and m434: 0.9.13 (the @r13 arm, its
auto level: cheap), the new cheap and full levels and the console (Dolphin's M35
capture, as tools/m44_water.py) at entry +1,200, four panels 400x300 each; m434
twice more, its two looks (--pondlook sky, the default, and tint).  Prints the
similarity to the console over the whole frame and the water's box (sim = 100 (1 -
mean |d| / 255), >8 = % of pixels whose worst channel differs by more than 8) and
writes OUTDIR/m45-water-GAME.jpg (+ m45-water-m434-looks.jpg) and m45-water.tsv."""
import glob
import os
import re
import sys

from PIL import Image, ImageChops, ImageDraw, ImageStat

HOME = os.path.expanduser("~")
CONSOLE = {"m417": 13494, "m405": 11737, "m434": 11959}  # the M35 capture at entry +1,200
BOX = {"m417": (60, 110, 520, 330), "m405": (0, 250, 640, 230), "m434": (160, 150, 330, 190)}
OFF = 1200
RUNS = {
    "m417": [("W-m417-@r13", "0.9.13 (cheap)"), ("W-m417-@w5,--water,cheap", "M45 cheap: no sky, ripple x2.5"),
             ("W-m417-@w7,--water,full", "M45 full: no sky, ripple x2.5")],
    "m405": [("W-m405-@r13", "0.9.13 (cheap)"), ("W-m405-@w7", "M45 cheap"), ("W-m405-@w7,--water,full", "M45 full")],
    "m434": [("W-m434-@r13", "0.9.13 (cheap)"), ("W-m434-@w7", "M45 cheap, pond 'sky'"),
             ("W-m434-@w7,--water,full", "M45 full, pond 'sky'")],
}
LOOKS = [("W-m434-@r13", "0.9.13 (cheap)"), ("W-m434-@w7", "M45 --pondlook sky (the default)"),
         ("W-m434-@w7,--pondlook,tint", "M45 --pondlook tint (no sky)"),
         ("W-m434-@w7,--water,full", "M45 full, sky"), ("W-m434-@w7,--water,full,--pondlook,tint", "M45 full, tint")]


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


def frame(src, run):
    d = os.path.join(src, run)
    e = entry_of(d + ".log") if os.path.exists(d + ".log") else None
    f = os.path.join(d, "frame-%05d.ppm" % (e + OFF)) if e else None
    return Image.open(f).convert("RGB") if f and os.path.exists(f) else None


def sheet(panels, g, path, cols=2):
    W, H = 400, 300
    rows = (len(panels) + cols - 1) // cols
    s = Image.new("RGB", (cols * W, rows * (H + 20)), (0, 0, 0))
    dr = ImageDraw.Draw(s)
    for i, (im, label) in enumerate(panels):
        x, y = (i % cols) * W, (i // cols) * (H + 20)
        if im is not None:
            s.paste(im.resize((W, H), Image.LANCZOS), (x, y + 20))
        dr.text((x + 6, y + 4), "%s  %s entry+%d" % (label, g, OFF), fill=(255, 255, 255))
    s.save(path, quality=85)
    print("wrote", path)


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    tsv = open(os.path.join(out, "m45-water.tsv"), "w")
    tsv.write("game\trun\tsim\t>8\tsim water\t>8 water\n")
    for g, runs in RUNS.items():
        c = os.path.join(HOME, "mp4-sweep-work/frames/%sm35/f%06d.png" % (g, CONSOLE[g]))
        cons = Image.open(c).convert("RGB").resize((640, 480), Image.BILINEAR) if os.path.exists(c) else None
        panels = []
        for run, label in runs:
            im = frame(src, run)
            panels.append((im, label))
            if im is not None and cons is not None:
                s1, o1 = stats(im, cons)
                s2, o2 = stats(im, cons, BOX[g])
                print("%s %-44s sim %.1f >8 %.1f%%   water: sim %.1f >8 %.1f%%" % (g, run, s1, o1, s2, o2))
                tsv.write("%s\t%s\t%.1f\t%.1f\t%.1f\t%.1f\n" % (g, run, s1, o1, s2, o2))
        panels.append((cons, "console (Dolphin)"))
        sheet(panels, g, os.path.join(out, "m45-water-%s.jpg" % g))
    cons = Image.open(os.path.join(HOME, "mp4-sweep-work/frames/m434m35/f%06d.png" % CONSOLE["m434"])).convert(
        "RGB").resize((640, 480), Image.BILINEAR)
    panels = [(frame(src, r), l) for r, l in LOOKS] + [(cons, "console (Dolphin)")]
    for (im, l), (r, _) in zip(panels, LOOKS):
        if im is not None:
            s1, o1 = stats(im, cons)
            s2, o2 = stats(im, cons, BOX["m434"])
            print("m434 %-44s sim %.1f >8 %.1f%%   water: sim %.1f >8 %.1f%%" % (r, s1, o1, s2, o2))
            tsv.write("m434\t%s\t%.1f\t%.1f\t%.1f\t%.1f\n" % (r, s1, o1, s2, o2))
    sheet(panels, "m434", os.path.join(out, "m45-water-m434-looks.jpg"))


if __name__ == "__main__":
    main()
