#!/usr/bin/env python3
"""M35 (PLAN.md 50.13, 50.14): pull the G4's ~/m35/tfs/NAME/*.ppm runs (m35_tfs.sh) and lay
each dumped position beside the M35 first-pass gallery frame (the "before") and the console's,
with the sim numbers of both against the console.

    m35_tfs_pull.py [--no-pull] NAME[=GAME] ...      e.g.  m35_tfs_pull.py m405 m417 m425old=m425

Writes docs/screenshots/m35-tfs-<NAME>-<pos>.jpg (before | after | console) and prints a table.
The position <-> frame number follows the gallery: e60 14537, e400 14877, e1200 15677
(the mgdump frames of a game with a card; m35_findings.py's NOCARD_PRE for the others)."""
import os, sys, glob, json, subprocess
from PIL import Image, ImageChops, ImageStat, ImageDraw
HOME = os.path.expanduser('~'); WORK = f'{HOME}/mp4-sweep-work'
HERE = os.path.dirname(os.path.abspath(__file__))
G = os.path.join(os.path.dirname(HERE), 'docs', 'gallery')
OUT = os.path.join(os.path.dirname(HERE), 'docs', 'screenshots')
LOCAL = f'{HOME}/m35-tfs'
args = [x for x in sys.argv[1:] if not x.startswith('--')]
pull = '--no-pull' not in sys.argv
before_tag = 'm35'; REMOTE = 'm35/tfs'
for x in sys.argv[1:]:
    if x.startswith('--before='): before_tag = x[9:]
    if x.startswith('--remote='): REMOTE = x[9:]
types = {l.split()[0]: int(l.split()[2]) for l in open(f'{WORK}/types.txt') if l.strip()}
POS = {'e60': 14537, 'e400': 14877, 'e1200': 15677}
NOCARD_PRE = {'e60': '01420', 'e400': '01454', 'e1200': '01534'}

def picks(g):
    P = None
    for t in ('', '-r', '-rtc', '-r2'):
        pk = f'{WORK}/picks/{g}{t}.json'
        if os.path.exists(pk):
            Q = json.load(open(pk))
            if P is None or len(Q['picks']) >= len(P['picks']): P = Q
    return P
def sim(A, B):
    A = A.convert('RGB').resize((320, 240)); B = B.convert('RGB').resize((320, 240))
    d = ImageChops.difference(A, B); mad = sum(ImageStat.Stat(d).mean) / 3
    r, g_, b_ = d.split(); m = ImageChops.lighter(ImageChops.lighter(r, g_), b_)
    return 100.0 * (1 - mad / 255.0), sum(1 for v in m.getdata() if v > 8) * 100.0 / 76800
def frame_of(files, g, k, tag=None):
    nocard = types.get(g, 0) in (3, 5, 6, 8)
    pre = NOCARD_PRE[k] if nocard else f'{POS[k]:06d}'[:-1]
    for f in sorted(files):
        b = os.path.basename(f)
        num = ''.join(ch for ch in (b.split('-f')[-1] if '-f' in b else b.replace('frame-', '')).split('.')[0] if ch.isdigit())
        if f'{int(num):06d}'[:5] == pre: return f
    return None
rows = []
for spec in args:
    name, _, game = spec.partition('=')
    game = game or name
    d = f'{LOCAL}/{name}'; os.makedirs(d, exist_ok=True)
    if pull:
        subprocess.call(['scp', '-q', f'g4:{REMOTE}/{name}/*.ppm', d + '/'])
        subprocess.call(['scp', '-q', f'g4:{REMOTE}/{name}.log', f'{LOCAL}/{name}.log'])
    ppms = glob.glob(f'{d}/frame-*.ppm')
    P = picks(game)
    for k in ('e60', 'e400', 'e1200'):
        pf = frame_of(ppms, game, k)
        bf = frame_of(glob.glob(f'{G}/{game}-{before_tag}-f*.jpg'), game, k)
        cf = f'{G}/oracle/{game}-c{P["picks"][k]:06d}.jpg' if (P and k in P.get('picks', {})) else None
        if not pf: continue
        A = Image.open(pf).convert('RGB').resize((320, 240), Image.LANCZOS)
        A.save(pf[:-4] + '.jpg', 'JPEG', quality=92)
        B = Image.open(bf).convert('RGB') if bf else None
        C = Image.open(cf).convert('RGB') if cf and os.path.exists(cf) else None
        sb = sim(B, C) if (B and C) else None; sa = sim(A, C) if C else None
        W = Image.new('RGB', (3 * 324, 256), 'black'); dr = ImageDraw.Draw(W)
        caps = [f'before ({before_tag}) {os.path.basename(bf) if bf else "none"}' + (f' sim {sb[0]:.0f}%/>8 {sb[1]:.0f}%' if sb else ''),
                f'after ({name}) {os.path.basename(pf)}' + (f' sim {sa[0]:.0f}%/>8 {sa[1]:.0f}%' if sa else ''),
                f'console {os.path.basename(cf) if cf else "none"}']
        for i, im in enumerate([B, A, C]):
            if im: W.paste(im, (324 * i, 16))
            dr.text((324 * i + 2, 2), caps[i][:62], fill='white')
        os.makedirs(OUT, exist_ok=True)
        W.save(f'{OUT}/m35-tfs-{name}-{k}.jpg', 'JPEG', quality=85)
        rows.append((name, game, k, sb, sa))
print('| run | game | position | before sim/>8 | after sim/>8 |')
print('|---|---|---|---|---|')
for name, game, k, sb, sa in rows:
    f = lambda s: f'{s[0]:.0f}%/{s[1]:.0f}%' if s else '-'
    print(f'| {name} | {game} | {k} | {f(sb)} | {f(sa)} |')
