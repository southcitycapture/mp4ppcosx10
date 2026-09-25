#!/usr/bin/env python3
"""M44 (PLAN.md 59): the water's cost per screen and level.

    m44_water_cost.py DIR [SCOREBOARD.md]

DIR holds the chain's W:GAME:ARM runs (W-GAME-ARM--water,LEVEL.csv/.log, in
lockstep: every frame drawn).  Per game and level: the drawn frame's medians
over the module's entry +300..+1,500 (the game thread's work, its GX share,
the render thread's replay and decode) and the level's cost against off.
With the scoreboard (M43's), each screen's real-time cycle on the reference
(the game thread's drawn work + a consumed frame's, the render thread's
replay + decode) plus the level's cost, against the 33.3 ms of a 30 fps
cycle less a 2 ms margin: the levels that fit."""
import csv
import glob
import os
import re
import statistics as st
import sys

LEVELS = ['off', 'cheap', 'full']
BUDGET = 33.3 - 2.0


def entry(log):
    for line in open(log, errors='replace'):
        m = re.search(r'entered minigame \S+ \(mg \d+\) at frame (\d+)', line)
        if m:
            return int(m.group(1))
    return None


def med(rows, k):
    return st.median(float(r[k]) for r in rows) if rows else float('nan')


def board(md):
    out = {}
    if not md or not os.path.exists(md):
        return out
    for line in open(md):
        p = [x.strip() for x in line.split('|')]
        if len(p) > 12 and p[1].endswith('dll'):
            try:
                g, c = p[7].split('/')[0].strip(), p[8]
                out[p[1][:-3]] = (float(p[3]), float(g), float(c), float(p[9]), float(p[10]))
            except ValueError:
                pass
    return out


def main():
    d = sys.argv[1]
    sb = board(sys.argv[2] if len(sys.argv) > 2 else None)
    res = {}
    for c in glob.glob(os.path.join(d, 'W-*--water,*.csv')):
        m = re.match(r'W-(m\d+)-.*--water,(\w+)\.csv$', os.path.basename(c))
        if not m:
            continue
        g, lv = m.groups()
        e = entry(c[:-4] + '.log')
        if e is None:
            continue
        rows = [r for r in csv.DictReader(open(c)) if e + 300 <= int(r['frame']) < e + 1500 and r['drawn'] == '1']
        res[(g, lv)] = (med(rows, 'work_ms'), med(rows, 'gx_ms'), med(rows, 'rt_ms'), med(rows, 'dec_ms'))
    print('| game | level | drawn work | GX | rt | dec | cost (game / rt) | reference cycle + cost (game / rt) | fits |')
    print('|---|---|---:|---:|---:|---:|---:|---:|---|')
    for g in sorted(set(k[0] for k in res)):
        off = res.get((g, 'off'))
        for lv in LEVELS:
            r = res.get((g, lv))
            if not r or not off:
                continue
            cg, cr = r[0] - off[0], (r[2] + r[3]) - (off[2] + off[3])
            fit = ''
            cyc = ''
            if g in sb:
                fps, gw, cons, rt, dec = sb[g]
                a, b = gw + cons + cg, rt + dec + cr
                cyc = '%.1f / %.1f' % (a, b)
                fit = 'yes' if a <= BUDGET and b <= BUDGET else 'no'
            print('| %s | %s | %.1f | %.1f | %.1f | %.1f | %+.1f / %+.1f | %s | %s |' % (
                g, lv, r[0], r[1], r[2], r[3], cg, cr, cyc, fit))


if __name__ == '__main__':
    main()
