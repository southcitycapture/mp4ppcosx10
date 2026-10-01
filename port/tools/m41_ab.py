#!/usr/bin/env python3
"""M41 (PLAN.md 56): the levers' A/B -- tools/m41_chain.sh's A:GAME:ARM:K
runs, per scene and arm: the scene's median presented fps per run (its own
module's status lines, real time only), and over the runs the drawn frame's
medians from the CSVs (the game thread's work, gdec, the consumed frame, rt,
dec, vertices, the cache's share).

    m41_ab.py DIR        (DIR holds A-*.log and A-*.csv, plain or .gz)
"""
import sys, os, re, glob, csv, gzip, statistics as st
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fps_board

SCREEN = {'cs': 'mentdll'}


def main():
    d = sys.argv[1]
    runs = {}
    for p in sorted(glob.glob(os.path.join(d, 'A-*.log*'))):
        m = re.match(r'A-(\w+?)-(.+)-(\d+)\.log', os.path.basename(p))
        if not m:
            continue
        g, arm, k = m.groups()
        screen = SCREEN.get(g, g + 'dll')
        by, cost = fps_board.read([p])
        v = by.get(screen, [])
        if not v:
            continue
        fps = st.median(x[1] for x in v)
        c = fps_board.costs(cost.get(screen, []))
        runs.setdefault((g, arm), []).append((int(k), fps, c))
    arms = ['base'] + sorted(set(k[1] for k in runs) - {'base'})
    print('| scene | arm | presented fps, each run | mean | game work / gdec | consumed | rt | dec | vertices | vc |')
    print('|---|---|---|---:|---:|---:|---:|---:|---:|---:|')
    for g in sorted(set(k[0] for k in runs)):
        for a in arms:
            r = runs.get((g, a))
            if not r:
                continue
            r.sort()
            cs = [x[2] for x in r if x[2]]
            mm = lambda key: st.mean(c[key] for c in cs if c.get(key) is not None) if cs else None
            f = lambda x: '-' if x is None else '%.1f' % x
            print('| %s | %s | %s | **%.1f** | %s / %s | %s | %s | %s | %s | %s |' % (
                SCREEN.get(g, g), a, ' / '.join('%.1f' % x[1] for x in r), st.mean(x[1] for x in r),
                f(mm('work')), f(mm('gdec')), f(mm('cons')), f(mm('rt')), f(mm('dec')),
                '%.0f' % mm('verts') if mm('verts') is not None else '-',
                '%.0f%%' % mm('vc') if mm('vc') is not None else '-'))


if __name__ == '__main__':
    main()
