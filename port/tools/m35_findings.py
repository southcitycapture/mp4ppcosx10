#!/usr/bin/env python3
"""M35 (PLAN.md 50): the user's review findings (~/review-findings/*.json: gid, label,
severity, comment, thumb = the frame they marked with their strokes) laid beside the
port's re-dumped frame and the console's, with the sim numbers before and after.

    m35_findings.py [--tag m35] [--out DIR] [--table FILE]

For each finding: docs/screenshots/m35-finding-<gid>-<n>.jpg = the marked frame (the
frame they reviewed, 320x240) | the newest port frame of that position | the console
frame, and one table row: gid, label, severity, comment, sim before -> after.
The port frame's position -> file number follows compare_m35.py (PORTF / NOCARD_PRE);
the console frame comes from the sweep's picks (~/mp4-sweep-work/picks/mNNN*.json)."""
import os, sys, json, glob, base64, io, argparse
from PIL import Image, ImageChops, ImageStat, ImageDraw
HOME = os.path.expanduser('~'); WORK = f'{HOME}/mp4-sweep-work'
HERE = os.path.dirname(os.path.abspath(__file__))
G = os.path.join(os.path.dirname(HERE), 'docs', 'gallery')
ap = argparse.ArgumentParser()
ap.add_argument('--tag', default='m35'); ap.add_argument('--findings', default=f'{HOME}/review-findings')
ap.add_argument('--out', default=os.path.join(os.path.dirname(HERE), 'docs', 'screenshots'))
ap.add_argument('--table', default=None)
a = ap.parse_args()
sys.path.insert(0, HERE)
TAGS = [a.tag, 'm34', 'm31', 'm30']
types = {l.split()[0]: int(l.split()[2]) for l in open(f'{WORK}/types.txt') if l.strip()}
PORTF = {'card1': 14200, 'card2': 14300, 'card3': 14400, 'e60': 14537, 'e400': 14877, 'e1200': 15677, 'e2300': 16777}
NOCARD_PRE = {'e60': '01420', 'e400': '01454', 'e1200': '01534', 'e2300': '01644'}
LABKEY = {'card +64': 'card1', 'card +164': 'card2', 'card +264': 'card3', 'entry +60': 'e60', 'entry +400': 'e400',
          'entry +1200': 'e1200', 'entry +2300': 'e2300', 'results': 'results'}
def key_of(label):
    for k, v in LABKEY.items():
        if label.startswith(k): return v
    return None
def port_frames(g, tag):
    files = sorted(glob.glob(f'{G}/{g}-{tag}-f*.jpg'))
    pre_len = len(g) + len(tag) + 3
    out = {}; used = set(); nocard = types[g] in (3, 5, 6, 8)
    for k in ['card1', 'card2', 'card3', 'e60', 'e400', 'e1200', 'e2300']:
        pre = NOCARD_PRE[k] if (nocard and k in NOCARD_PRE) else f'{PORTF[k]:06d}'[:-1]
        c = [f for f in files if os.path.basename(f)[pre_len:pre_len + 5] == pre and f not in used and not (nocard and k == 'e60' and f.endswith('014200.jpg'))]
        if c: out[k] = c[0]; used.add(c[0])
    rest = [f for f in files if f not in used and int(f[-10:-4]) > 14500]
    if rest: out['results'] = rest[0]
    return out
def newest(g):
    for t in TAGS:
        if glob.glob(f'{G}/{g}-{t}-f*.jpg'): return t
    return 'fix'
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
rows = []
for f in sorted(glob.glob(f'{a.findings}/*.json')):
    d = json.load(open(f)); g = d['gid']; k = key_of(d['label']); n = os.path.basename(f)[:-5]
    thumb = Image.open(io.BytesIO(base64.b64decode(d['thumb'].split(',', 1)[1]))).convert('RGB') if d.get('thumb') else None
    tag = newest(g); PF = port_frames(g, tag); pf = PF.get(k) if k else None
    P = picks(g); cf = f'{G}/oracle/{g}-c{P["picks"][k]:06d}.jpg' if (P and k in P.get('picks', {})) else None
    if cf and not os.path.exists(cf): cf = None
    before = d.get('sim', '')
    after = ''
    if pf and cf:
        s, o = sim(Image.open(pf), Image.open(cf)); after = f'sim {s:.0f}% · >8: {o:.0f}%'
    W = Image.new('RGB', (3 * 324, 256), 'black'); dr = ImageDraw.Draw(W)
    for i, (im, cap) in enumerate([(thumb, f'{n}: the frame the user marked ({before})'),
                                   (Image.open(pf).convert('RGB').resize((320, 240)) if pf else None, f'port {tag}: {os.path.basename(pf) if pf else "none"} ({after})'),
                                   (Image.open(cf).convert('RGB').resize((320, 240)) if cf else None, f'console {os.path.basename(cf) if cf else "none"}')]):
        if im: W.paste(im, (324 * i, 16))
        dr.text((324 * i + 2, 2), cap[:60], fill='white')
    os.makedirs(a.out, exist_ok=True)
    W.save(os.path.join(a.out, f'm35-finding-{n}.jpg'), 'JPEG', quality=85)
    rows.append((n, g, d.get('game', ''), d['label'], d.get('severity', ''), d.get('comment', '').replace('\n', ' / '), before, after, tag))
lines = ['| finding | game | position | severity | the user | sim before | sim after (build) |', '|---|---|---|---|---|---|---|']
for n, g, gn, lab, sev, com, b, af, tag in rows:
    lines.append(f'| {n} | {g} {gn} | {lab} | {sev or "-"} | {com} | {b.replace("sim ", "").replace(" · >8: ", "/") if b else "-"} | {(af.replace("sim ", "").replace(" · >8: ", "/") + " (" + tag + ")") if af else "-"} |')
out = '\n'.join(lines) + '\n'
if a.table: open(a.table, 'w').write(out)
print(out)
