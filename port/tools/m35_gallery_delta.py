#!/usr/bin/env python3
"""M35 second pass (PLAN.md 50.15): every gallery row's sim against the console, the first-pass
final build (~/gallery-m35a, the 9ac4788f run) against the second-pass final (~/gallery-m35g
pulled into ~/gallery-m35), and the pixels that moved between the two builds.

    m35_gallery_delta.py [--a DIR] [--b DIR] [--min-moved PCT]

Prints one row per (game, position) where the two builds differ, with sim/>8 against the
console for each and the fraction of pixels that moved between them, then the totals."""
import os, sys, glob, json
from PIL import Image, ImageChops, ImageStat
HOME = os.path.expanduser('~'); WORK = f'{HOME}/mp4-sweep-work'
HERE = os.path.dirname(os.path.abspath(__file__))
G = os.path.join(os.path.dirname(HERE), 'docs', 'gallery')
A = f'{HOME}/gallery-m35a'; B = f'{HOME}/gallery-m35'; MINMOVED = 0.05
args = sys.argv[1:]
for i, x in enumerate(args):
    if x == '--a': A = args[i + 1]
    if x == '--b': B = args[i + 1]
    if x == '--min-moved': MINMOVED = float(args[i + 1])
types = {l.split()[0]: int(l.split()[2]) for l in open(f'{WORK}/types.txt') if l.strip()}
POS = {'card1': 14200, 'card2': 14300, 'card3': 14400, 'e60': 14537, 'e400': 14877, 'e1200': 15677, 'e2300': 16777}
NOCARD_PRE = {'e60': '01420', 'e400': '01454', 'e1200': '01534', 'e2300': '01644'}
def picks(g):
    P = None
    for t in ('', '-r', '-rtc', '-r2'):
        pk = f'{WORK}/picks/{g}{t}.json'
        if os.path.exists(pk):
            Q = json.load(open(pk))
            if P is None or len(Q['picks']) >= len(P['picks']): P = Q
    return P
def sim(A_, B_):
    d = ImageChops.difference(A_, B_); mad = sum(ImageStat.Stat(d).mean) / 3
    r, g_, b_ = d.split(); m = ImageChops.lighter(ImageChops.lighter(r, g_), b_)
    return 100.0 * (1 - mad / 255.0), sum(1 for v in m.getdata() if v > 8) * 100.0 / 76800
def frame_of(d, g, k):
    nocard = types.get(g, 0) in (3, 5, 6, 8)
    pre = NOCARD_PRE[k] if (nocard and k in NOCARD_PRE) else f'{POS[k]:06d}'[:-1]
    for f in sorted(glob.glob(f'{d}/{g}/frame-*.ppm')):
        num = ''.join(ch for ch in os.path.basename(f).split('.')[0] if ch.isdigit())
        if f'{int(num):06d}'[:5] == pre and not (nocard and k == 'e60' and num.endswith('14200')): return f
    return None
def load(f): return Image.open(f).convert('RGB').resize((320, 240), Image.LANCZOS)
rows = []; n_same = n_moved = 0; sum_a = sum_b = 0.0; n_sim = 0
for g in sorted(os.listdir(B)):
    if not (g.startswith('m4') and os.path.isdir(f'{B}/{g}')): continue
    P = picks(g)
    for k in POS:
        fa = frame_of(A, g, k); fb = frame_of(B, g, k)
        if not fa or not fb: continue
        ia, ib = load(fa), load(fb)
        d = ImageChops.difference(ia, ib); r, g_, b_ = d.split(); m = ImageChops.lighter(ImageChops.lighter(r, g_), b_)
        moved = sum(1 for v in m.getdata() if v > 8) * 100.0 / 76800
        cf = f'{G}/oracle/{g}-c{P["picks"][k]:06d}.jpg' if (P and k in P.get('picks', {})) else None
        sa = sb = None
        if cf and os.path.exists(cf):
            ic = load(cf); sa = sim(ia, ic); sb = sim(ib, ic); sum_a += sa[0]; sum_b += sb[0]; n_sim += 1
        if moved >= MINMOVED:
            n_moved += 1; rows.append((g, k, moved, sa, sb))
        else:
            n_same += 1
f = lambda s: f'{s[0]:.0f}%/{s[1]:.0f}%' if s else '-'
print('| game | position | pixels moved | before (first-pass final) sim/>8 | after (second-pass final) sim/>8 |')
print('|---|---|---|---|---|')
for g, k, moved, sa, sb in rows:
    print(f'| {g} | {k} | {moved:.1f}% | {f(sa)} | {f(sb)} |')
print(f'\n{n_same} positions byte-identical to the level (< {MINMOVED}% moved), {n_moved} moved; '
      f'mean sim against the console {sum_a / max(n_sim, 1):.2f}% -> {sum_b / max(n_sim, 1):.2f}% over {n_sim} positions')
