#!/usr/bin/env python3
"""M52 (PLAN.md 67): what the projected shadow pass costs, minigame by minigame.

    m52_shadow.py DIR [--arm SUBSTR]

From tools/m52_chain.sh's SH: runs (lockstep, every frame drawn, --perf
--perfdump --blobaudit): per minigame, over entry +300..+900, the medians of
the game thread's time in Hu3DShadowExec (shd_ms), the render thread's replay
of it (shd_rt_ms), the drawn frame (work_ms) and the render thread's frame
(rt_ms), and the casters the audit saw at +600 (shown / all).  Sorted by the
shadow pass's larger half, heaviest first."""
import csv, glob, gzip, os, re, statistics as st, sys


def opn(p):
    return gzip.open(p, 'rt', errors='replace') if p.endswith('.gz') else open(p, errors='replace')


def main():
    d = sys.argv[1]
    arm = sys.argv[sys.argv.index('--arm') + 1] if '--arm' in sys.argv else ''
    rows = []
    for p in sorted(glob.glob(os.path.join(d, 'SH-m*.log*'))):
        b = os.path.basename(p)
        if arm and arm not in b:
            continue
        m = re.match(r'SH-(m\d+)-', b)
        g = m.group(1)
        entry = None
        cast = {}
        for line in opn(p):
            e = re.search(r'entered minigame \S+ \(mg \d+\) at frame (\d+)', line)
            if e and entry is None:
                entry = int(e.group(1))
            a = re.search(r'blobaudit mg \d+ f\+(\d+) model (\d+) CASTER( hookfunc)?( dispoff)?', line)
            if a and int(a.group(1)) in (600, 601, 602):
                cast[int(a.group(2))] = a.group(4) is None
        c = re.sub(r'\.log(\.gz)?$', '.csv', p)
        if not os.path.exists(c):
            c += '.gz'
        if entry is None or not os.path.exists(c):
            continue
        r = [x for x in csv.DictReader(opn(c)) if entry + 300 <= int(x['frame']) < entry + 900 and x['drawn'] == '1']
        if not r or 'shd_ms' not in r[0]:
            continue
        f = lambda k: st.median(float(x[k]) for x in r)
        rows.append((g, f('shd_ms'), f('shd_rt_ms'), f('work_ms'), f('rt_ms'), sum(cast.values()), len(cast)))
    rows.sort(key=lambda x: -max(x[1], x[2]))
    print('| minigame | shadow pass, game thread (ms) | its replay, render thread (ms) | drawn frame (ms) | render thread (ms) | casters shown / all |')
    print('|---|---:|---:|---:|---:|---:|')
    for g, a, b, w, rt, n, t in rows:
        print('| %s | %.2f | %.2f | %.1f | %.1f | %d / %d |' % (g, a, b, w, rt, n, t))


if __name__ == '__main__':
    main()
