#!/usr/bin/env python3
"""M44 (PLAN.md 59): the A/B of tools/m44_chain.sh's A:GAME:ARM:K runs.

    m44_ab.py DIR [DIR...]

Per scene and arm: the scene's median presented fps per run (fps_board.py's
reading of its own status lines, real time only), their median; and over the
runs, from the CSVs, the medians in the module's window (entry +300..+1,500;
the boards and the character select: their scoreboard windows) of the drawn
frame's work, of EVERY consumed frame's work (fps_board.py's consumed column
takes only the consumed frames over 10 ms of wall -- a few percent of them),
of the render thread's replay and decode."""
import csv
import glob
import os
import re
import statistics as st
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fps_board  # noqa: E402

SCREEN = {'cs': 'mentdll', 'b1': 'w01dll', 'b2': 'w02dll', 'b3': 'w03dll', 'b4': 'w04dll',
          'b5': 'w05dll', 'b6': 'w06dll'}
WIN = {'cs': (3000, 4800), 'b': (8400, 10400)}


def window(g, log):
    if g == 'cs':
        return WIN['cs']
    if g.startswith('b'):
        return WIN['b']
    for line in open(log, errors='replace'):
        m = re.search(r'entered minigame \S+ \(mg \d+\) at frame (\d+)', line)
        if m:
            e = int(m.group(1))
            return (e + 300, e + 1500)
    return None


def main():
    runs = {}
    for d in sys.argv[1:]:
        for p in sorted(glob.glob(os.path.join(d, 'A-*.log'))):
            m = re.match(r'A-(\w+?)-(.+)-(\d+)\.log$', os.path.basename(p))
            if not m:
                continue
            g, arm, k = m.groups()
            screen = SCREEN.get(g, g + 'dll')
            by, _ = fps_board.read([p])
            v = by.get(screen, [])
            if not v:
                continue
            fps = st.median(x[1] for x in v)
            w = window(g, p)
            c = p[:-4] + '.csv'
            dr = cr = []
            if w and os.path.exists(c):
                rows = [r for r in csv.DictReader(open(c)) if w[0] <= int(r['frame']) < w[1]]
                dr = [r for r in rows if r['drawn'] == '1']
                cr = [r for r in rows if r['drawn'] == '0']
            runs.setdefault((g, arm), []).append((int(k), fps, dr, cr))
    print('| scene | arm | presented fps, each run | median | drawn work | consumed work (all) | rt | dec |')
    print('|---|---|---|---:|---:|---:|---:|---:|')
    for g in sorted(set(k[0] for k in runs)):
        for a in sorted(set(k[1] for k in runs if k[0] == g), key=lambda x: (x != 'old', x)):
            r = sorted(runs[(g, a)])
            dr = [x for q in r for x in q[2]]
            cr = [x for q in r for x in q[3]]
            f = lambda rows, k: st.median(float(x[k]) for x in rows) if rows else float('nan')
            print('| %s | %s | %s | **%.1f** | %.1f | %.1f | %.1f | %.1f |' % (
                g, a, ' / '.join('%.1f' % q[1] for q in r), st.median(q[1] for q in r),
                f(dr, 'work_ms'), f(cr, 'work_ms'), f(dr, 'rt_ms'), f(dr, 'dec_ms')))


if __name__ == '__main__':
    main()
