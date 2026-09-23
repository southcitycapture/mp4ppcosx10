#!/usr/bin/env python3
"""The 30-fps scoreboard (the v1.0 bar, PLAN.md §55): one table of every screen
the given logs visit, worst first.

    fps_board.py LOG [LOG ...] [--bar 29.5] [--min-lines 10] [--md OUT.md]
                 [--title T] [--before OTHER.md-or-logs...]

Reads --status lines (gz or plain) from any number of logs -- a soak, a gallery
chain's per-game logs, a --goto teleport per screen, tools/fps_board.sh's chain
-- and pools them by screen: a minigame module (m4xx), a board module
(w0x/w10/w2x, all its turns pooled), or any other overlay (the title, file
select, mode select, character select, results, story, options, credits...).
Per screen: status lines, the MEDIAN and the 10th percentile of presented fps,
the mean game speed, the render thread's rt and dec medians where the log
carries them, and PASS when the median meets the bar AND the mean speed is at
least 99%.  Screens with fewer than --min-lines status lines are listed
separately as too short to judge.

M40: a log NAME.log with a NAME.csv beside it (--perfdump; tools/fps_board.sh
writes one per run) adds the drawn frame's costs per screen, each a median
over the screen's drawn frames: the game thread's whole drawn frame (work) and
its decode share (gdec), the consumed frame's work, the render thread's replay
(rt) and decode (dec), and what the frame handed GL -- draw calls, vertices,
stream records (every GL call) -- with the share of vertices the static-
geometry cache served (vc).  A CSV row belongs to the screen of the first
status line at or after its frame.  Status lines of a fast-forward (speed
over 110%, --ffto's turbo stretch) are not the player's and are left out.

The definition of done, as the user set it: "30fps everything", measured as a
median of at least 29.5 presented fps at 100% game speed on the dual 1 GHz G4.
"""
import argparse, bisect, csv, gzip, os, re, statistics as st, sys

ST = re.compile(r'status f(\d+)\s+(\S+)\s+board (\d+) turn (\d+)/(\d+)\s+mg (\d+) \((\S+)\).*?'
                r'speed (\d+)%\s+([\d.]+) fps presented(?:.*?rt ([\d.]+) ms dec ([\d.]+))?')


def pct(xs, p):
    xs = sorted(xs)
    if not xs:
        return 0.0
    k = (len(xs) - 1) * p
    lo = int(k); hi = min(lo + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def opn(path):
    return gzip.open(path, 'rt', errors='replace') if path.endswith('.gz') else open(path, errors='replace')


def csv_of(path):
    stem = path[:-3] if path.endswith('.gz') else path
    stem = stem[:-4] if stem.endswith('.log') else stem
    for c in (stem + '.csv', stem + '.csv.gz'):
        if os.path.exists(c):
            return c
    return None


def read(paths, fastcut=110):
    by = {}      # screen -> status tuples
    cost = {}    # screen -> list of csv rows (dicts)
    for path in paths:
        marks = []   # (frame, screen) of this log's status lines
        for line in opn(path):
            m = ST.search(line)
            if not m:
                continue
            f, ovl, _, _, _, _, _, speed, fps, rt, dec = m.groups()
            marks.append((int(f), ovl))
            if int(speed) > fastcut:
                continue
            by.setdefault(ovl, []).append((int(speed), float(fps),
                                           float(rt) if rt else None, float(dec) if dec else None))
        c = csv_of(path)
        if not c or not marks:
            continue
        frames = [x[0] for x in marks]
        for r in csv.DictReader(opn(c)):
            try:
                fr = int(r['frame'])
            except (KeyError, ValueError):
                continue
            i = bisect.bisect_left(frames, fr)
            if i >= len(marks):
                continue
            cost.setdefault(marks[i][1], []).append(r)
    return by, cost


def costs(rows):
    if not rows:
        return None
    dr = [r for r in rows if r.get('drawn') == '1']
    cr = [r for r in rows if r.get('drawn') == '0' and float(r.get('wall_ms', 0)) > 10.0]
    if not dr:
        return None
    f = lambda k, rs=dr: st.median(float(r[k]) for r in rs) if rs and k in rs[0] else None
    out = dict(work=f('work_ms'), game=f('game_ms'), gdec=f('gdec_ms'), rt=f('rt_ms'),
               dec=f('dec_ms'), cons=f('work_ms', cr) if cr else None, drawn=len(dr))
    if 'calls' in dr[0]:
        out['calls'] = f('calls'); out['verts'] = f('verts'); out['recs'] = f('recs')
        v = sum(float(r['verts']) for r in dr)
        out['vc'] = 100.0 * sum(float(r['vchit']) for r in dr) / v if v else 0.0
    return out


def table(by, cost, bar, min_lines):
    rows = []; short = []
    for k, v in by.items():
        fps = [x[1] for x in v]; spd = [x[0] for x in v]
        rt = [x[2] for x in v if x[2] is not None]; dec = [x[3] for x in v if x[3] is not None]
        r = dict(screen=k, n=len(v), med=st.median(fps), p10=pct(fps, 0.10), speed=st.mean(spd),
                 rt=st.median(rt) if rt else None, dec=st.median(dec) if dec else None,
                 c=costs(cost.get(k, [])))
        r['pass'] = r['med'] >= bar and r['speed'] >= 99.0
        (rows if len(v) >= min_lines else short).append(r)
    rows.sort(key=lambda r: (r['med'], r['p10']))
    return rows, short


def fmt(x, p=1):
    return '-' if x is None else ('%.*f' % (p, x))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--bar', type=float, default=29.5)
    ap.add_argument('--min-lines', type=int, default=10)
    ap.add_argument('--md')
    ap.add_argument('--title', default='30 fps scoreboard')
    ap.add_argument('--fastcut', type=int, default=110)
    a = ap.parse_args()
    by, cost = read(a.logs, a.fastcut)
    rows, short = table(by, cost, a.bar, a.min_lines)
    npass = sum(r['pass'] for r in rows)
    withc = any(r['c'] for r in rows)
    out = ['# %s\n' % a.title]
    out.append('Bar: median presented fps >= %.1f at >= 99%% game speed. Logs: %d (%s).\n' % (
        a.bar, len(a.logs), ', '.join(sorted(set(os.path.dirname(x) or '.' for x in a.logs)))))
    out.append('**%d of %d screens pass.**\n' % (npass, len(rows)))
    if withc:
        out.append('Per drawn frame (medians): the game thread\'s work and its decode share (gdec); the '
                   'consumed frame\'s work; the render thread\'s replay (rt) and decode (dec); draw calls, '
                   'vertices and GL calls (stream records) handed to GL; vc = vertices the static-geometry '
                   'cache served.\n')
        out.append('| screen | lines | median fps | p10 fps | speed | game work / gdec ms | consumed ms | '
                   'rt ms | dec ms | draws | vertices | GL calls | vc | verdict |')
        out.append('|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|')
    else:
        out.append('| screen | lines | median fps | p10 fps | speed | rt ms | dec ms | verdict |')
        out.append('|---|---:|---:|---:|---:|---:|---:|---|')
    for r in rows:
        verdict = ('PASS' if r['pass'] else 'short by %.1f fps' % max(0.0, a.bar - r['med'])
                   if r['speed'] >= 99.0 else 'SPEED %.1f%%' % r['speed'])
        c = r['c']
        if withc:
            out.append('| %s | %d | %.1f | %.1f | %.1f%% | %s | %s | %s | %s | %s | %s | %s | %s | %s |' % (
                r['screen'], r['n'], r['med'], r['p10'], r['speed'],
                ('%s / %s' % (fmt(c['work']), fmt(c['gdec']))) if c else '-',
                fmt(c['cons']) if c else '-',
                fmt(c['rt'] if c else r['rt']), fmt(c['dec'] if c else r['dec']),
                fmt(c.get('calls'), 0) if c else '-', fmt(c.get('verts'), 0) if c else '-',
                fmt(c.get('recs'), 0) if c else '-',
                ('%.0f%%' % c['vc']) if c and c.get('vc') is not None else '-', verdict))
        else:
            out.append('| %s | %d | %.1f | %.1f | %.1f%% | %s | %s | %s |' % (
                r['screen'], r['n'], r['med'], r['p10'], r['speed'], fmt(r['rt']), fmt(r['dec']), verdict))
    if short:
        out.append('\nToo few status lines to judge (< %d): %s' % (a.min_lines, ', '.join(
            '%s (%d)' % (r['screen'], r['n']) for r in sorted(short, key=lambda r: r['screen']))))
    text = '\n'.join(out) + '\n'
    if a.md:
        open(a.md, 'w').write(text)
    sys.stdout.write(text)


if __name__ == '__main__':
    main()
