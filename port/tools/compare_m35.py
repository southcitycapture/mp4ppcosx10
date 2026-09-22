#!/usr/bin/env python3
"""M35 (PLAN.md 50), from M34's compare_m34.py: regenerate port/docs/gallery/compare.html
with the newest build's frames per game -- mNNN-m31-f*.jpg over mNNN-m30-f*.jpg over the
M26 fix build's -- each tag with its own verdicts-TAG.tsv (game, verdict, note).  The console frames stay; a game with
mNNN-m30-fNNNNNN.jpg frames in the gallery dir uses those in place of the M26
fix build's, its verdict and note come from verdicts-m30.tsv (game, verdict, note),
and the header counts are recomputed.  Everything else is M26b's compare.py
(~/mp4-sweep-work/compare.py) unchanged; the sweep's picks (which console frame
is which event) are read from ~/mp4-sweep-work/picks/.

    compare_m35.py [--gallery-dir DIR]     (default: port/docs/gallery next to this file)
"""
import os, sys, json, glob
from PIL import Image, ImageChops, ImageStat
HOME = os.path.expanduser('~'); WORK = f'{HOME}/mp4-sweep-work'
HERE = os.path.dirname(os.path.abspath(__file__))
G = os.path.join(os.path.dirname(HERE), 'docs', 'gallery')
if len(sys.argv) > 2 and sys.argv[1] == '--gallery-dir': G = sys.argv[2]
KEYS = ['card1', 'card2', 'card3', 'e60', 'e400', 'e1200', 'e2300', 'results']
LABEL = {'card1': 'card +64 (walk 14200)', 'card2': 'card +164 (14300)', 'card3': 'card +264 (14400)', 'e60': 'entry +60', 'e400': 'entry +400',
         'e1200': 'entry +1200', 'e2300': 'entry +2300', 'results': 'results'}
PORTF = {'card1': 14200, 'card2': 14300, 'card3': 14400, 'e60': 14537, 'e400': 14877, 'e1200': 15677, 'e2300': 16777}
names = {}
for line in open(f'{HOME}/gallery-tools/names.tsv'):
    p = line.rstrip('\n').split('\t')
    if len(p) >= 2 and p[0].startswith('m4'): names[p[0]] = p[1]
types = {l.split()[0]: int(l.split()[2]) for l in open(f'{WORK}/types.txt') if l.strip()}
TYPE = {0: '4-player', 1: '1-vs-3', 2: '2-vs-2', 3: 'Bowser', 4: 'Battle', 5: 'Item', 6: 'Story', 7: 'Extra', 8: 'Story/Extra'}
old = {}
for line in open(f'{HOME}/gallery-tools/verdicts.tsv'):
    p = line.rstrip('\n').split('\t')
    if len(p) >= 2 and p[0].startswith('m4'): old[p[0]] = p[1]
verd = {}
for line in open(f'{WORK}/verdicts.tsv'):
    p = line.rstrip('\n').split('\t')
    if len(p) >= 3 and p[0].startswith('m4'): verd[p[0]] = (p[1], p[2], p[3] if len(p) > 3 else '')
TAGS = ['m35', 'm34', 'm31', 'm30']   # newest first; 'fix' (the M26 fix build) is the floor
tagv = {}
for t in TAGS:
    tagv[t] = {}
    vp = os.path.join(G, f'verdicts-{t}.tsv')
    if os.path.exists(vp):
        for line in open(vp):
            p = line.rstrip('\n').split('\t')
            if len(p) >= 3 and p[0].startswith('m4'): tagv[t][p[0]] = (p[1], p[2])

NOCARD_PRE = {'e60': '01420', 'e400': '01454', 'e1200': '01534', 'e2300': '01644'}
def port_frames(g, tag):
    files = sorted(glob.glob(f'{G}/{g}-{tag}-f*.jpg')); out = {}; used = set()
    nocard = types[g] in (3, 5, 6, 8)
    for k in KEYS[:7]:
        if nocard and k in NOCARD_PRE: pre = NOCARD_PRE[k]
        else: pre = f'{PORTF[k]:06d}'[:-1]
        c = [f for f in files if os.path.basename(f)[len(g) + len(tag) + 3:len(g) + len(tag) + 8] == pre and f not in used and not (nocard and k == 'e60' and f.endswith('014200.jpg'))]
        if c: out[k] = c[0]; used.add(c[0])
    rest = [f for f in files if f not in used and int(f[-10:-4]) > 14500]
    if rest: out['results'] = rest[0]
    return out
def sim(a, b):
    A = Image.open(a).convert('RGB').resize((320, 240)); B = Image.open(b).convert('RGB').resize((320, 240))
    d = ImageChops.difference(A, B)
    mad = sum(ImageStat.Stat(d).mean) / 3
    r, g_, b_ = d.split(); m = ImageChops.lighter(ImageChops.lighter(r, g_), b_)
    over = sum(1 for v in m.getdata() if v > 8) * 100.0 / (320 * 240)
    return 100.0 * (1 - mad / 255.0), over

H = []
H.append('''<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Mario Party 4 PowerPC port: the port beside the console, every minigame (M26b, M30, M31, M34, M35)</title>
<style>body{font:13px/1.35 -apple-system,Helvetica,Arial,sans-serif;margin:8px;background:#fafafa}
.g{border-top:2px solid #ccc;padding:8px 0}.h{font-weight:bold;font-size:16px}.m{color:#555;font-size:12px}
.f{display:flex;flex-wrap:wrap;gap:6px;margin:6px 0}.f figure{margin:0;width:240px;background:#fff;border:1px solid #ddd;padding:3px}
.f img{width:240px;height:180px;display:block}.f figcaption{font-size:11px;color:#444}.f .lab{font-weight:bold;color:#222}
.n{background:#fff;border:1px solid #ddd;padding:6px;font-size:13px;margin-top:4px}.miss{width:240px;height:180px;background:#eee;color:#888;display:flex;align-items:center;justify-content:center;font-size:11px}
.match{color:#2a7}.minor{color:#c80}.major{color:#c22}.port-faulted{color:#808}.oracle-failed{color:#777}.m30{background:#ffd}
table{border-collapse:collapse;font-size:12px}td,th{border:1px solid #ccc;padding:2px 5px;text-align:left}
@media(max-width:700px){.f figure{width:48%}.f img{width:100%;height:auto}}</style>
<h1>The port beside the console: every minigame, seven positions (M26b, the oracle sweep; M30, M31, M34 and M35 rows regenerated)</h1>
<p>Each row: the port's frame (top: the M26 fix build on the G4, or &mdash; on the rows marked <span class="m30">M30</span> / <span class="m30">M31</span> / <span class="m30">M34</span> / <span class="m30">M35</span> &mdash; that milestone's build, <code>gallery_chain.sh</code>) over the console's (bottom, Dolphin on littlejelly, the same schedule: board-start walk, four COMs, <code>mg_next</code> and <code>mgNext</code> poked, groups by type) at the same <em>event</em>: the instruction card at instDll +64/+164/+264, the module at entry +60/+400/+1200/+2300, and the results screen 40 frames after the module is left. The console frame's number is Dolphin's dump index. Under each pair: <b>sim</b> = 100·(1 − mean |Δ| / 255) over RGB at 320×240, <b>&gt;8</b> = % of pixels whose worst channel differs by more than 8 levels (ppmdiff's count). The two runs do not share their RNG (the board seeds from the clock at board setup, which the two walks reach at different frames), so play frames differ by the play itself; the card and the intro frames are the frame-exact pairs. PLAN.md §41b has the calibration and the verdicts' reasoning; §45 the M30 rows, §46 the M31 rows, §49 the M34 rows, §50 the M35 rows (each regenerated row keeps the earlier verdicts and notes below its own as the before picture).</p>''')
counts = {}; tsv = open(os.path.join(G, 'similarity-m35.tsv'), 'w'); tsv.write('game\t' + '\t'.join(KEYS) + '\n')
for g in sorted(types):
    P = None
    for t in ('', '-r', '-rtc', '-r2'):
        pk = f'{WORK}/picks/{g}{t}.json'
        if os.path.exists(pk):
            Q = json.load(open(pk))
            if P is None or len(Q['picks']) >= len(P['picks']): P = Q
    v26, note26, box = verd.get(g, ('?', '', ''))
    tag = 'fix'
    for t in TAGS:
        if glob.glob(f'{G}/{g}-{t}-f*.jpg'): tag = t; break
    v, note = v26, note26
    if tag != 'fix':
        # the newest tag's verdict, else the older tag's (a game re-dumped without a new verdict)
        for t in TAGS[TAGS.index(tag):]:
            if g in tagv[t]: v, note = tagv[t][g]; break
    prev = [f'<b>{t.upper()} ({tagv[t][g][0]})</b>: {tagv[t][g][1]}' for t in TAGS[TAGS.index(tag) + 1:] if tag != 'fix' and g in tagv[t]] if tag != 'fix' else []
    counts[v] = counts.get(v, 0) + 1
    H.append(f'<div class="g" id="{g}"><div class="h">{g} &mdash; {names.get(g, g)} <span class="m">({TYPE.get(types[g], types[g])}, type {types[g]})</span> '
             f'<span class="m">§41: {old.get(g, "?")} &rarr; M26b: {v26} &rarr;</span> <span class="{v}">{v}</span>' + (f' <span class="m30">{tag.upper()} build</span>' if tag != 'fix' else '') + '</div>')
    if P:
        H.append(f'<div class="m">console: card {P.get("I_c")} (first yellow frame {P.get("card0")}, delta {P.get("delta")}), module first frame {P.get("S_c")}, '
                 f'entry gc {P.get("entry_gc")}, left gc {P.get("left_gc")}; stop: {P.get("reason")}{"; MISDEAL " + str(P.get("misdeal")) if P.get("misdeal") else ""}</div>')
    H.append('<div class="f">')
    sims = []; PF = port_frames(g, tag)
    for k in KEYS:
        pf = PF.get(k); cf = None
        if P and k in P['picks']: cf = f'{G}/oracle/{g}-c{P["picks"][k]:06d}.jpg'
        if not pf and not cf: sims.append(''); continue
        pmiss = 'port: faulted before this frame' if v == 'port-faulted' else 'port: none (the game ran on)'
        cmiss = 'console: not captured'
        if P and P.get('left_gc') and k != 'results': cmiss = 'console: its game was over by then'
        if P and P.get('misdeal'): cmiss = 'console: misdealt'
        if v == 'oracle-failed': cmiss = 'console: hung at the module'
        s = ''
        if pf and cf and os.path.exists(cf):
            a, b = sim(pf, cf); s = f'sim {a:.0f}% &middot; &gt;8: {b:.0f}%'; sims.append(f'{a:.0f}/{b:.0f}')
        else: sims.append('-')
        H.append('<figure>')
        H.append(f'<figcaption class="lab">{LABEL[k]}</figcaption>')
        H.append(f'<img loading="lazy" src="{os.path.basename(pf)}" alt="{g} port {k}"><figcaption>port {os.path.basename(pf)[len(g)+len(tag)+2:-4]}</figcaption>' if pf else f'<div class="miss">{pmiss}</div>')
        H.append(f'<img loading="lazy" src="oracle/{os.path.basename(cf)}" alt="{g} console {k}"><figcaption>console {os.path.basename(cf)[len(g)+1:-4]}</figcaption>' if cf and os.path.exists(cf) else f'<div class="miss">{cmiss}</div>')
        H.append(f'<figcaption>{s}</figcaption></figure>')
    H.append('</div>')
    extra = ''.join('<br>' + p for p in prev) + (f'<br><b>M26b ({v26})</b>: {note26}' if tag != 'fix' else '')
    H.append(f'<div class="n"><b class="{v}">{v}</b>: {note}' + extra + (f'<br><b>preview box</b>: {box}' if box else '') + '</div></div>')
    tsv.write(g + '\t' + '\t'.join(sims) + '\n')
summary = ', '.join(f'<span class="{k}">{k}: {n}</span>' for k, n in sorted(counts.items()))
H.insert(1, f'<p>{summary} &mdash; {len(types)} games</p>')
open(os.path.join(G, 'compare.html'), 'w').write('\n'.join(H) + '\n')
print('written', counts)
