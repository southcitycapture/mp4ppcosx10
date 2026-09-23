#!/usr/bin/env python3
"""The 30-fps scoreboard (the v1.0 bar, PLAN.md §55): one table of every screen
the given logs visit, worst first.

    fps_board.py LOG [LOG ...] [--bar 29.5] [--min-lines 10] [--md OUT.md]

Reads --status lines (gz or plain) from any number of logs -- a soak, a gallery
chain's per-game logs, a --goto teleport per screen -- and pools them by screen:
a minigame module (m4xx), a board module (w0x/w10/w2x, all its turns pooled), or
any other overlay (the title, file select, mode select, character select,
results, story, options, credits...). Per screen: status lines, the MEDIAN and
the 10th percentile of presented fps, the mean game speed, the render thread's
rt and dec medians where the log carries them, and PASS when the median meets
the bar AND the mean speed is at least 99%. Screens with fewer than --min-lines
status lines are listed separately as too short to judge.

The definition of done, as the user set it: "30fps everything", measured as a
median of at least 29.5 presented fps at 100% game speed on the dual 1 GHz G4.
"""
import argparse, gzip, re, statistics as st, sys

ST = re.compile(r'status f(\d+)\s+(\S+)\s+board (\d+) turn (\d+)/(\d+)\s+mg (\d+) \((\S+)\).*?'
                r'speed (\d+)%\s+([\d.]+) fps presented(?:.*?rt ([\d.]+) ms dec ([\d.]+))?')

def pct(xs, p):
    xs = sorted(xs)
    if not xs: return 0.0
    k = (len(xs) - 1) * p
    lo = int(k); hi = min(lo + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)

def screen_of(ovl):
    return ovl  # boards pool across turns because the key is the module

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--bar', type=float, default=29.5)
    ap.add_argument('--min-lines', type=int, default=10)
    ap.add_argument('--md')
    a = ap.parse_args()
    by = {}
    for path in a.logs:
        f = gzip.open(path, 'rt', errors='replace') if path.endswith('.gz') else open(path, errors='replace')
        for line in f:
            m = ST.search(line)
            if not m: continue
            _, ovl, _, _, _, _, _, speed, fps, rt, dec = m.groups()
            by.setdefault(screen_of(ovl), []).append((int(speed), float(fps),
                                                      float(rt) if rt else None, float(dec) if dec else None))
    rows = []; short = []
    for k, v in by.items():
        fps = [x[1] for x in v]; spd = [x[0] for x in v]
        rt = [x[2] for x in v if x[2] is not None]; dec = [x[3] for x in v if x[3] is not None]
        r = dict(screen=k, n=len(v), med=st.median(fps), p10=pct(fps, 0.10), speed=st.mean(spd),
                 rt=st.median(rt) if rt else None, dec=st.median(dec) if dec else None)
        r['pass'] = r['med'] >= a.bar and r['speed'] >= 99.0
        (rows if len(v) >= a.min_lines else short).append(r)
    rows.sort(key=lambda r: (r['med'], r['p10']))
    npass = sum(r['pass'] for r in rows)
    out = []
    out.append('# 30 fps scoreboard\n')
    out.append('Bar: median presented fps >= %.1f at >= 99%% game speed. Logs: %s.\n' % (a.bar, ', '.join(a.logs)))
    out.append('**%d of %d screens pass.**\n' % (npass, len(rows)))
    out.append('| screen | lines | median fps | p10 fps | speed | rt ms | dec ms | verdict |')
    out.append('|---|---:|---:|---:|---:|---:|---:|---|')
    for r in rows:
        out.append('| %s | %d | %.1f | %.1f | %.1f%% | %s | %s | %s |' % (
            r['screen'], r['n'], r['med'], r['p10'], r['speed'],
            '%.1f' % r['rt'] if r['rt'] is not None else '-', '%.1f' % r['dec'] if r['dec'] is not None else '-',
            'PASS' if r['pass'] else 'short by %.1f fps' % max(0.0, a.bar - r['med']) if r['speed'] >= 99.0 else 'SPEED %.1f%%' % r['speed']))
    if short:
        out.append('\nToo few status lines to judge (< %d): %s' % (a.min_lines, ', '.join(
            '%s (%d)' % (r['screen'], r['n']) for r in sorted(short, key=lambda r: r['screen']))))
    text = '\n'.join(out) + '\n'
    if a.md: open(a.md, 'w').write(text)
    sys.stdout.write(text)

if __name__ == '__main__':
    main()
