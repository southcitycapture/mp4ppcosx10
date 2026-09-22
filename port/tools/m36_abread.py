#!/usr/bin/env python3
"""M36 (PLAN.md 51): read the loader's A/B (port/tools/m36_ab.sh).

    m36_abread.py DIR       a directory of ARM<run>.log from the chain

Per arm: the cold minigame-load stall (every `DVD: read of …` over 100 ms
and the `stall:` lines at a module link), the resync count and the game time
they dropped, the speed, the presented fps, the loader's own totals and the
peak RSS.  The mean of the arm's runs, then the per-run rows.
"""
import re, sys, os, statistics as st, collections

d = sys.argv[1]
arms = collections.OrderedDict()
for fn in sorted(os.listdir(d)):
    m = re.match(r'(P\dR\d)(\d)\.log$', fn)
    if not m:
        continue
    arm, run = m.group(1), m.group(2)
    r = dict(run=run, slow=[], resyncs=0, resync_s=0.0, stalls=[], speed=None, fps=None,
             rss=0, served=0, disk=0, pre_files=0, pre_mb=0.0, res_mb=0, mg=0, dvd_ms=0.0,
             dvd_reads=0, dvd_over=0, slices=0, late_ms=0.0)
    for line in open(os.path.join(d, fn), errors='replace'):
        m2 = re.search(r'DVD: read of (\S+) \((\d+) bytes at (\d+)\) took (\d+) ms \(frame (\d+)\)', line)
        if m2:
            r['slow'].append((m2.group(1), int(m2.group(4)), int(m2.group(5))))
        m2 = re.search(r'realtime: resync at retrace (\d+), (\d+) ms behind', line)
        if m2:
            r['resyncs'] += 1; r['resync_s'] += int(m2.group(2)) / 1000.0
        m2 = re.search(r'stall: frame (\d+) took (\d+) ms', line)
        if m2 and int(m2.group(2)) >= 300:
            r['stalls'].append((int(m2.group(1)), int(m2.group(2))))
        m2 = re.search(r'clocks\s+game ([\d.]+) s vs wall ([\d.]+) s -- speed ([\d.]+)%', line)
        if m2:
            r['speed'] = float(m2.group(3))
        m2 = re.search(r'present\s+(\d+) drawn of (\d+) \([\d.]+% skipped\), ([\d.]+) presented fps', line)
        if m2:
            r['fps'] = float(m2.group(3))
        m2 = re.search(r'rss (\d+) MB', line)
        if m2:
            r['rss'] = max(r['rss'], int(m2.group(1)))
        m2 = re.search(r'res (\d+)/(\d+) MB', line)
        if m2:
            r['res_mb'] = max(r['res_mb'], int(m2.group(1)))
        m2 = re.search(r'DVD: (\d+) reads, (\d+) bytes, (\d+) ms in reads, (\d+) over 100 ms', line)
        if m2:
            r['dvd_reads'] = int(m2.group(1)); r['dvd_ms'] = float(m2.group(3)); r['dvd_over'] = int(m2.group(4))
        m2 = re.search(r'(\d+) files read ahead .*?([\d.]+) MB in ([\d.]+) ms off the game thread', line)
        if m2:
            r['pre_files'] = int(m2.group(1)); r['pre_mb'] = float(m2.group(2))
        m2 = re.search(r'(\d+) inline slices', line)
        if m2:
            r['slices'] = int(m2.group(1))
        m2 = re.search(r'(\d+) reads \(([\d.]+) MB\) served from memory, (\d+) \(', line)
        if m2:
            r['served'] = int(m2.group(1)); r['disk'] = int(m2.group(3))
        if 'soak: enter minigame' in line:
            r['mg'] += 1
        m2 = re.search(r'late\s+worst ([\d.]+) ms behind the schedule', line)
        if m2:
            r['late_ms'] = float(m2.group(1))
    arms.setdefault(arm, []).append(r)

NAME = {'P0R0': 'neither (--noprefetch --resident 0)',
        'P1R0': 'prefetch only (--resident 0)',
        'P1R1': 'both (the defaults)'}
print("%-34s %3s %5s %6s %6s %7s %7s %7s %6s %5s" % (
    "arm", "run", "mg", "slow", "worst", "resyncs", "lost s", "DVD ms", "speed", "rss"))
summary = {}
for arm, runs in arms.items():
    for r in runs:
        print("%-34s %3s %5d %6d %6d %7d %7.2f %7.0f %5s%% %5d" % (
            NAME.get(arm, arm), r['run'], r['mg'], len(r['slow']),
            max([x[1] for x in r['slow']], default=0), r['resyncs'], r['resync_s'],
            r['dvd_ms'], r['speed'] if r['speed'] is not None else '-', r['rss']))
    summary[arm] = runs
print()
print("%-34s %6s %7s %7s %8s %8s %7s %7s" % (
    "arm (mean of the runs)", "slow", "worst", "resyncs", "lost s", "DVD ms", "fps", "rss"))
for arm, runs in arms.items():
    ok = [r for r in runs if r['speed'] is not None]
    print("%-34s %6.1f %7.0f %7.1f %8.2f %8.0f %7.1f %7d" % (
        NAME.get(arm, arm),
        st.mean(len(r['slow']) for r in runs),
        st.mean(max([x[1] for x in r['slow']], default=0) for r in runs),
        st.mean(r['resyncs'] for r in runs),
        st.mean(r['resync_s'] for r in runs),
        st.mean(r['dvd_ms'] for r in runs),
        st.mean(r['fps'] for r in ok) if ok else 0.0,
        max(r['rss'] for r in runs)))
print()
for arm, runs in arms.items():
    for r in runs:
        if r['slow']:
            print("%s%s slow reads: %s" % (arm, r['run'],
                  ', '.join('%s %d ms @f%d' % x for x in r['slow'][:12])))
        if r['pre_files'] or r['served']:
            print("%s%s loader: %d files read ahead (%.1f MB), %d reads served from memory, %d from the disk, "
                  "%d MB resident, %d inline slices" % (arm, r['run'], r['pre_files'], r['pre_mb'],
                  r['served'], r['disk'], r['res_mb'], r['slices']))
