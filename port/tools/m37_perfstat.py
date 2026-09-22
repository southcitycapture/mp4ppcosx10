#!/usr/bin/env python3
"""M37: the same --perfdump CSV as m33_perfstat.py, read for a *small*
difference.

Presented fps on the character select is a coin flip: the scene's cycle sits
on the boundary between two and three retraces (§48.2), so the walk's fps is
a mixture of 30 and 20 and moves a whole frame a second between repeats of
the same arm. The stable numbers are underneath it -- the work per cycle and
the share of cycles that took the third retrace -- and they are medians and
means over hundreds of frames rather than a count of 370.

    m37_perfstat.py LABEL:CSV [LABEL:CSV ...]
"""
import csv, sys, statistics as st

WIN = {"title": (700, 870), "charsel": (2600, 3600), "board": (6000, 8900)}


def read(path):
    return list(csv.DictReader(open(path)))


def cycles(w):
    """(retraces, total work ms) per drawn-frame cycle in the window."""
    idx = [i for i, r in enumerate(w) if r['drawn'] == '1']
    out = []
    for a, b in zip(idx, idx[1:]):
        out.append((b - a, sum(float(r['work_ms']) for r in w[a:b])))
    return out


def main():
    arms = {}
    for a in sys.argv[1:]:
        label, path = a.split(':', 1)
        arms.setdefault(label, []).append(read(path))
    for name, (lo, hi) in WIN.items():
        print("%s:" % name)
        for label, runs in arms.items():
            three = work = fps = drawn = 0.0
            cyc = []
            rt = []
            game = []
            for rows in runs:
                w = [r for r in rows if lo <= int(r['frame']) <= hi]
                c = cycles(w)
                cyc += c
                d = [r for r in w if r['drawn'] == '1']
                rt += [float(r['rt_ms']) for r in d]
                game += [float(r['game_ms']) for r in d]
                wall = sum(float(r['wall_ms']) for r in w) / 1000.0
                fps += len(d) / wall if wall else 0
                drawn += len(d)
            n = len(runs)
            three = sum(1 for r, _ in cyc if r >= 3) / float(len(cyc)) if cyc else 0
            work = st.mean(x for _, x in cyc) if cyc else 0
            print("  %-10s runs %d  cycle %.3f retraces (%.1f%% took a third)  "
                  "work/cycle %.2f ms  rt %.2f  game %.2f  presented %.2f" %
                  (label, n, st.mean(r for r, _ in cyc) if cyc else 0, 100 * three,
                   work, st.median(rt), st.median(game), fps / n))


if __name__ == '__main__':
    main()
