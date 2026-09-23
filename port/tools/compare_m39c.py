#!/usr/bin/env python3
"""M39c (PLAN.md 54b.5): the board rows of port/docs/gallery/compare.html.

Reads ~/mp4-board-oracle/w0N/picks.json (m39c_oracle.py pick: port frame,
console dump index, match distance), the port's survey frames in
~/m39c-work/SN/frame-NNNNN.ppm and the console's ~/mp4-board-oracle/w0N/cNNNNNN.png,
writes 320x240 JPEGs to port/docs/gallery/boards/ (w0N-pFFFFF.jpg,
w0N-cCCCCCC.jpg), and puts a "boards" section at the end of compare.html
between <!-- m39c boards --> markers (replaced on every run, so the script
is idempotent and compare_m39.py's regeneration can be followed by this).
Verdicts and notes: port/docs/gallery/verdicts-m39c-boards.tsv (board, verdict, note).
"""
import os, json, glob
from PIL import Image, ImageChops, ImageStat

HOME = os.path.expanduser('~')
HERE = os.path.dirname(os.path.abspath(__file__))
G = os.path.join(os.path.dirname(HERE), 'docs', 'gallery')
OUT = os.path.join(G, 'boards')
NAMES = {1: "Toad's Midway Madness", 2: "Goomba's Greedy Gala", 3: "Shy Guy's Jungle Jam",
         4: "Boo's Haunted Bash", 5: "Koopa's Seaside Soir&eacute;e", 6: "Bowser's Gnarly Party"}
LABELS = ['the intro flyover (entry +250)', "the host's first Star (a board event)",
          'turn 1: the first player on the board', "turn 1's end: the minigame roulette"]
verd = {}
vp = os.path.join(G, 'verdicts-m39c-boards.tsv')
if os.path.exists(vp):
    for line in open(vp):
        p = line.rstrip('\n').split('\t')
        if len(p) >= 3 and p[0].startswith('w0'):
            verd[p[0]] = (p[1], p[2])


def sim(a, b):
    A = a.convert('RGB').resize((320, 240)); B = b.convert('RGB').resize((320, 240))
    d = ImageChops.difference(A, B)
    s = 100 * (1 - sum(ImageStat.Stat(d).mean) / 3 / 255)
    px = d.load()
    big = sum(1 for y in range(240) for x in range(320) if max(px[x, y]) > 8) * 100 / (320 * 240)
    return s, big


os.makedirs(OUT, exist_ok=True)
rows = []
counts = {}
for n in range(1, 7):
    b = f'w0{n}'
    pj = f'{HOME}/mp4-board-oracle/{b}/picks.json'
    if not os.path.exists(pj):
        continue
    picks = json.load(open(pj))
    figs = []
    for k, (f, c, dist) in enumerate(picks):
        pp = f'{HOME}/m39c-work/S{n}/frame-{f:05d}.ppm'
        P = Image.open(pp); C = Image.open(f'{HOME}/mp4-board-oracle/{b}/c{c:06d}.png')
        pn = f'{b}-p{f:05d}.jpg'; cn = f'{b}-c{c:06d}.jpg'
        P.convert('RGB').resize((320, 240)).save(os.path.join(OUT, pn), quality=82)
        C.convert('RGB').resize((320, 240)).save(os.path.join(OUT, cn), quality=82)
        s, big = sim(P, C)
        figs.append(f'<figure>\n<figcaption class="lab">{LABELS[k]}</figcaption>\n'
                    f'<img loading="lazy" src="boards/{pn}" alt="{b} port"><figcaption>port f{f:06d}</figcaption>\n'
                    f'<img loading="lazy" src="boards/{cn}" alt="{b} console"><figcaption>console c{c:06d}</figcaption>\n'
                    f'<figcaption>sim {s:.0f}% &middot; &gt;8: {big:.0f}%</figcaption></figure>')
    v, note = verd.get(b, ('?', ''))
    counts[v] = counts.get(v, 0) + 1
    rows.append(f'<div class="g" id="{b}"><div class="h">{b} &mdash; {NAMES[n]} <span class="m">(board {n}, '
                f'<code>--board {n}</code>)</span></div>\n<div class="f">\n' + '\n'.join(figs) +
                f'\n</div>\n<div class="n"><b class="{v}">{v}</b>: {note}</div></div>')

head = ('<!-- m39c boards -->\n<h1 id="boards">The boards (M39c): the six party boards, four moments each</h1>\n'
        '<p>Each board: the port (top: <code>--board N --com4 --rtc dolphin --freshcard --nomovies --play '
        'board-start-com4.play</code> at lockstep on the G4, a frame every 250 from the board\'s entry, '
        '<code>tools/m39c_chain.sh</code> phase B) over the console (bottom: Dolphin on littlejelly, '
        '<code>ref/movies/board-com4-oracle.txt</code> &mdash; the same walk, four COMs, GWSystem.board poked '
        'while mentDll runs, <code>tools/m39c_oracle.py</code>) at the same <em>event</em>: the board\'s title '
        'card over the intro flyover (250 frames after the board overlay is entered), the host showing the first '
        'Star, the first player\'s turn, and turn 1\'s end (the minigame roulette). The console\'s frame is '
        'found by content, in order, in the Dolphin dump of the board. The two walks reach the board 584 frames '
        'apart, so the board\'s RNG differs: the Star\'s space, the turn order and the minigame are the play; '
        'the title card, the host, the board, the HUD and the dialogs are what the numbers compare. sim and &gt;8 '
        'as above. PLAN.md &sect;54b.5.</p>\n'
        '<p>' + ', '.join(f'<span class="{k}">{k}: {v}</span>' for k, v in sorted(counts.items())) +
        f' &mdash; {len(rows)} boards</p>\n')
section = head + '\n'.join(rows) + '\n<!-- /m39c boards -->\n'

cp = os.path.join(G, 'compare.html')
s = open(cp).read()
a = s.find('<!-- m39c boards -->')
if a >= 0:
    e = s.find('<!-- /m39c boards -->')
    s = s[:a] + s[e + len('<!-- /m39c boards -->\n'):]
s = s.rstrip('\n') + '\n' + section
if 'M39c boards' not in s[:600]:
    s = s.replace('(M26b, M30, M31, M34, M35, M39)</title>', '(M26b, M30, M31, M34, M35, M39) and every board (M39c)</title>', 1)
    s = s.replace('</h1>\n<p>Each row:', '</h1>\n<p><a href="#boards">The six party boards (M39c boards) are at the end.</a></p>\n<p>Each row:', 1)
open(cp, 'w').write(s)
print(f'{len(rows)} board rows; {counts}')
