#!/usr/bin/env python3
"""M45 (PLAN.md 60): the picture checks -- a build's frames against the previous
release's, so a picture regression the md5 walks never reach cannot ship again.

    python3 port/tools/m45_piccheck.py REF_DIR NEW_DIR [--out DIR]

REF_DIR and NEW_DIR are two `PC:` directories of tools/m45_chain.sh (pulled from
the G4: pc-ARM/NAME/frame-N.ppm, pc-ARM/NAME.log).  Runs are paired by name with
the arm stripped (L-m427-@c14 <-> L-m427-@w5).  For every frame both dumped:
identical (md5), or how it differs (sim = 100 (1 - mean |d| / 255), the share of
pixels whose worst channel moved by more than 8, the mean brightness of each
half).  A FAIL is:
  * a half of the new frame at most 40% as bright as the reference's (a black
    or dropped view: m427's left half on 0.9.13), or the whole frame so;
  * a half-black frame or a blip counted by --halfwatch in a new run (the
    reference is a pre-M45 build without the counter, or its own counts);
  * a new run that exits non-zero or faults.
Frames that differ without failing are listed for a look (the water's tuning
moves m427's river on purpose).  Writes OUT/piccheck.tsv and, for every failing
or differing frame, OUT/NAME-frame-N.jpg (reference | new | the difference x4).
Exit status 1 if anything failed."""
import glob
import hashlib
import os
import re
import sys

from PIL import Image, ImageChops, ImageStat


def md5(path):
    return hashlib.md5(open(path, "rb").read()).hexdigest()[:8]


def key(name):
    # L-m427-@c14 / R-m427-@w5,--flag-1 / T-item3-@w5 -> L-m427 / R-m427-1 / T-item3
    m = re.match(r"^([A-Z]+-[a-z0-9]+)-(?:@[^-]*|old|base)(?:,[^/]*?)?(-\d+)?$", name)
    if m:
        return m.group(1) + (m.group(2) or "")
    return name


def runs(d):
    out = {}
    for p in sorted(glob.glob(os.path.join(d, "*"))):
        if os.path.isdir(p):
            out[key(os.path.basename(p))] = p
    return out


def halfwatch(log):
    hb = bl = None
    fault = 0
    exitbad = False
    if not os.path.exists(log):
        return hb, bl, fault, True
    for line in open(log, errors="replace"):
        m = re.search(r"halfwatch: \d+ frames checked \(every \d+\), (\d+) half-black(?:, (\d+) blips)?", line)
        if m:
            hb = int(m.group(1))
            bl = int(m.group(2) or 0)
        if line.startswith("*** port"):
            fault += 1
        if "over the" in line and "ceiling" in line:
            exitbad = True
    return hb, bl, fault, exitbad


def halves(im):
    l = sum(ImageStat.Stat(im.crop((0, 0, 320, 480))).mean) / 3.0
    r = sum(ImageStat.Stat(im.crop((320, 0, 640, 480))).mean) / 3.0
    return l, r


def main():
    ref, new = sys.argv[1], sys.argv[2]
    out = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else os.path.join(new, "piccheck")
    os.makedirs(out, exist_ok=True)
    R, N = runs(ref), runs(new)
    tsv = open(os.path.join(out, "piccheck.tsv"), "w")
    tsv.write("run\tframe\tverdict\tsim\t>8%\tref L/R\tnew L/R\n")
    n_frames = n_same = n_diff = 0
    fails = []
    for k in sorted(N):
        hb, bl, fault, bad = halfwatch(N[k] + ".log")
        if hb:
            fails.append("%s: %d half-black frame(s) (--halfwatch)" % (k, hb))
        if bl:
            fails.append("%s: %d blip(s) (--halfwatch)" % (k, bl))
        if fault or bad:
            fails.append("%s: %d fault(s)%s" % (k, fault, ", over its ceiling" if bad else ""))
        for f in sorted(glob.glob(os.path.join(N[k], "frame-*.ppm"))):
            n_frames += 1
            fr = os.path.basename(f)
            rf = os.path.join(R[k], fr) if k in R else None
            if not rf or not os.path.exists(rf):
                tsv.write("%s\t%s\tno reference\t\t\t\t\n" % (k, fr))
                continue
            if md5(f) == md5(rf):
                n_same += 1
                tsv.write("%s\t%s\tidentical\t100\t0\t\t\n" % (k, fr))
                continue
            n_diff += 1
            a = Image.open(rf).convert("RGB")
            b = Image.open(f).convert("RGB")
            d = ImageChops.difference(a, b)
            sim = 100.0 * (1.0 - sum(ImageStat.Stat(d).mean) / 3.0 / 255.0)
            px = d.getdata()
            over = 100.0 * sum(1 for p in px if max(p) > 8) / len(px)
            al, ar = halves(a)
            bl_, br = halves(b)
            verdict = "differs"
            if (al > 12 and bl_ < 0.4 * al) or (ar > 12 and br < 0.4 * ar):
                verdict = "FAIL (a half went dark)"
                fails.append("%s %s: a half went dark (%.1f/%.1f -> %.1f/%.1f)" % (k, fr, al, ar, bl_, br))
            tsv.write("%s\t%s\t%s\t%.1f\t%.1f\t%.1f/%.1f\t%.1f/%.1f\n" % (k, fr, verdict, sim, over, al, ar, bl_, br))
            sheet = Image.new("RGB", (960, 240))
            sheet.paste(a.resize((320, 240)), (0, 0))
            sheet.paste(b.resize((320, 240)), (320, 0))
            sheet.paste(Image.eval(d, lambda v: min(255, v * 4)).resize((320, 240)), (640, 0))
            sheet.save(os.path.join(out, "%s-%s.jpg" % (k, fr[:-4])), quality=80)
    print("picture checks: %d runs, %d frames, %d identical to the reference, %d differ, %d failures"
          % (len(N), n_frames, n_same, n_diff, len(fails)))
    for f in fails:
        print("  FAIL", f)
    print("  table: %s" % os.path.join(out, "piccheck.tsv"))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
