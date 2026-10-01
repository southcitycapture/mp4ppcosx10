#!/usr/bin/env python3
"""dolphin_map.py -- a Dolphin run of a session movie as a frame map for the
side-by-side video and the picture comparison (M51, PLAN.md 66.3/66.5).

  dolphin_map.py REC WATCH.csv OUT.json [--resync R_RES V_RES] [--until R]

From the run's own trace: the port's events matched in order from the boot
(dolphin_sync.match) and, with --resync, again from the hand-over at the
instruction card (the port's retrace R_RES = Dolphin's field V_RES).  Every
retrace gets the field of the last matched event at or before it plus the
frames since (`vmap`); `instep` lists the stretches where the events match
(from the first to the last matched event of each part) -- outside them the
two games are not the same and the video says so.
"""
import argparse, json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from rec2dtm import parse_rec, sig_timeline  # noqa: E402
from dolphin_sync import dolphin_trace, events, match, match_after, field_of  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument('rec')
ap.add_argument('watch')
ap.add_argument('out')
ap.add_argument('--resync', nargs=2, type=int)
ap.add_argument('--until', type=int)
a = ap.parse_args()
rec = parse_rec(a.rec)
end = min(rec['end'], a.until) if a.until else rec['end']
ps = sig_timeline(rec)[:end + 1]
dtr = dolphin_trace(a.watch)
pe = events(ps)
pairs1, fail1 = match(pe, events(dtr), ps, dtr)
parts = [(pairs1, fail1)]
if a.resync:
    r_res, v_res = a.resync
    pe2 = [e for e in pe if e[0] >= r_res]
    pairs2, fail2 = match_after(pe2, dtr, ps, (r_res, v_res))
    parts.append((pairs2, fail2))
allp = sorted(set(p for pr, _ in parts for p in pr))
vmap = {r: field_of(allp, r) for r in range(end + 1)}
instep = [[pr[0][0], pr[-1][0], (f[0] if f else None)] for pr, f in parts if pr]
json.dump({'vmap': {str(k): v for k, v in vmap.items()}, 'instep': instep,
           'events': [len(pr) for pr, _ in parts]}, open(a.out, 'w'))
for (pr, f), st in zip(parts, instep):
    print(f'in step: retraces {st[0]}..{st[1]} ({len(pr)} events), first miss {st[2]}')
