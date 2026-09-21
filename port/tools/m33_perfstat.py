#!/usr/bin/env python3
"""M33: medians of a --perfdump CSV per window, drawn and consumed apart (the §42.4 / §44 table's
columns): consumed (game, aud), drawn (game, gx, rt = the render thread's replay, dec = its
decode of the frame, M29), presented fps.  Older CSVs without dec_ms read 0."""
import csv, sys, statistics as st
WIN = {"title": (700, 870), "charsel": (2600, 3600), "board": (6000, 8900)}
def med(v): return st.median(v) if v else float('nan')
for path in sys.argv[1:]:
    rows = list(csv.DictReader(open(path)))
    out = [path.split('/')[-2] if '/' in path else path]
    for name, (a, b) in WIN.items():
        w = [r for r in rows if a <= int(r['frame']) <= b]
        d = [r for r in w if r['drawn'] == '1']; c = [r for r in w if r['drawn'] == '0']
        wall = sum(float(r['wall_ms']) for r in w) / 1000.0
        pf = len(d) / wall if wall else 0
        idx = [i for i, r in enumerate(w) if r['drawn'] == '1']
        gaps = [b - a for a, b in zip(idx, idx[1:])]
        cyc = med([sum(float(r['work_ms']) for r in w[a:b]) for a, b in zip(idx, idx[1:])])
        out.append("%s: consumed %.2f (game %.2f aud %.2f) drawn %.1f (game %.1f gx %.1f rt %.1f dec %.1f gdec %.1f) cycle %.1f retraces (game-thread work %.1f ms) presented %.2f [%d/%d]" % (
            name, med([float(r['work_ms']) for r in c]), med([float(r['game_ms']) for r in c]), med([float(r['aud_ms']) for r in c]),
            med([float(r['work_ms']) for r in d]), med([float(r['game_ms']) for r in d]), med([float(r['gx_ms']) for r in d]),
            med([float(r['rt_ms']) for r in d]), med([float(r.get('dec_ms', 0) or 0) for r in d]),
            med([float(r.get('gdec_ms', 0) or 0) for r in d]),
            (sum(gaps) / len(gaps)) if gaps else float('nan'), cyc, pf, len(d), len(w)))
    print("\n  ".join(out))
