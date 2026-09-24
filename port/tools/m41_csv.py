#!/usr/bin/env python3
"""M41: the drawn and consumed frames of a --perfdump CSV, medians of the
screen's own frames (from the minigame's entry, or --from/--to).

    python3 port/tools/m41_csv.py RUN.csv [RUN.log] [--from F --to F]"""
import csv, re, sys, statistics as st
args = sys.argv[1:]
f0, f1 = None, None
if "--from" in args:
    i = args.index("--from"); f0 = int(args[i + 1]); del args[i:i + 2]
if "--to" in args:
    i = args.index("--to"); f1 = int(args[i + 1]); del args[i:i + 2]
path = args[0]
log = args[1] if len(args) > 1 else path[:-4] + ".log"
if f0 is None:
    try:
        for line in open(log, errors="replace"):
            m = re.search(r"mgdump: entered minigame \S+ \(mg \d+\) at frame (\d+)", line)
            if m:
                f0 = int(m.group(1)); break
    except OSError:
        pass
f0 = f0 or 0
f1 = f1 or f0 + 1800
dr, co = [], []
for r in csv.DictReader(open(path)):
    fr = int(r["frame"])
    if fr < f0 or fr >= f1:
        continue
    (dr if r["drawn"] == "1" else co).append(r)
def med(rows, k):
    v = [float(r[k]) for r in rows if k in r and r[k] != ""]
    return st.median(v) if v else float("nan")
def mean(rows, k):
    v = [float(r[k]) for r in rows if k in r and r[k] != ""]
    return sum(v) / len(v) if v else float("nan")
print("%s frames %d..%d: %d drawn, %d consumed" % (path, f0, f1, len(dr), len(co)))
print("  drawn    work %.1f  game %.1f  gx %.1f  present %.1f  aud %.2f  gdec %.1f | rt %.1f dec %.1f | calls %d verts %d recs %d" % (
    med(dr, "work_ms"), med(dr, "game_ms"), med(dr, "gx_ms"), med(dr, "present_ms"), med(dr, "aud_ms"),
    med(dr, "gdec_ms"), med(dr, "rt_ms"), med(dr, "dec_ms"), med(dr, "calls"), med(dr, "verts"), med(dr, "recs")))
print("  consumed work %.1f  game %.1f  gx %.1f  aud %.2f" % (
    med(co, "work_ms"), med(co, "game_ms"), med(co, "gx_ms"), med(co, "aud_ms")))
print("  cycle (drawn + consumed work) %.1f ms" % (med(dr, "work_ms") + med(co, "work_ms")))
