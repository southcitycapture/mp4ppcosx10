#!/usr/bin/env python3
"""M49b (PLAN.md 64b.1): compare the character files' hook joints printed by
--jointaudit.  For each character, hook and file pair (m1/m2, m2/m3, m1/m3):
exact (every matrix bit the same at every motion time) or the largest
difference of the translation (world units) and where it is."""
import re, sys, collections
rx = re.compile(r'^joint c(\d) m(\d) mot(\d\d) k(\d) t(\S+) (\S+) ((?:[0-9a-f]{8} ){12})pos (\S+) (\S+) (\S+)')
d = collections.defaultdict(dict)   # (c, hook) -> {(mot,k): {file: (bits, pos)}}
for line in open(sys.argv[1]):
    m = rx.match(line)
    if not m:
        continue
    c, f, mot, k = int(m[1]), int(m[2]), int(m[3]), int(m[4])
    d[(c, m[6])].setdefault((mot, k), {})[f] = (m[7].split(), tuple(float(x) for x in (m[8], m[9], m[10])))
names = ['Mario', 'Luigi', 'Peach', 'Yoshi', 'Wario', 'DK', 'Daisy', 'Waluigi']
hooks = ['a-itemhook-r', 'a-itemhook-l', 'a-itemhook-fr', 'a-itemhook-fl', 'a-itemhook-body',
         'test11_tex_we-itemhook-r', 'test11_tex_we-ske_R_shoe1']
for a, b in ((1, 2), (2, 3)):
    print(f'== m{a} vs m{b}')
    for c in range(8):
        out = []
        for h in hooks:
            e = d.get((c, h))
            if not e:
                out.append(f'{h}: absent')
                continue
            same = n = 0; worst = (-1.0, None); first = None
            for key, fs in sorted(e.items()):
                if a in fs and b in fs:
                    n += 1
                    if fs[a][0] == fs[b][0]:
                        same += 1
                    elif first is None:
                        first = key
                    dist = max(abs(x - y) for x, y in zip(fs[a][1], fs[b][1]))
                    if dist > worst[0]:
                        worst = (dist, key)
            if same == n:
                out.append(f'{h}: EXACT ({n} poses)')
            else:
                out.append(f'{h}: {same}/{n} poses exact, first differing mot{first[0]:02d} k{first[1]}, '
                           f'max |dpos| {worst[0]:.2f} at mot{worst[1][0]:02d} k{worst[1][1]}')
        print(f'c{c} {names[c]}:\n    ' + '\n    '.join(out))

# m441's use of itemhook-r: the net point, hook matrix x (0, 0, 170) (main.c:1013-1016),
# in float32 as MTXMultVec computes it; exact where every bit of the three floats agrees
import struct
def f32(h):
    return struct.unpack('>f', bytes.fromhex(h))[0]
def r32(x):
    return struct.unpack('>f', struct.pack('>f', x))[0]
def net(bits):
    # MTXMultVec with v = (0, 0, 170): each row m0*0 + m1*0 + m2*170 + m3, rounded to float32
    # (approximately the G4's order; exact equality of the inputs implies exact equality here)
    m = [f32(x) for x in bits]
    return tuple(r32(r32(m[r * 4 + 2] * 170.0) + m[r * 4 + 3]) for r in range(3))
print('== m441 net point (a-itemhook-r x (0,0,170)), m1 vs m2')
for c in range(8):
    e = d.get((c, 'a-itemhook-r'), {})
    n = same = 0; worst = 0.0; col = collections.Counter()
    for key, fs in e.items():
        if 1 in fs and 2 in fs:
            n += 1
            a, b = net(fs[1][0]), net(fs[2][0])
            if a == b:
                same += 1
            worst = max(worst, max(abs(float(x) - float(y)) for x, y in zip(a, b)))
            for i in range(12):
                if fs[1][0][i] != fs[2][0][i]:
                    col[i % 4] += 1
    print(f'c{c} {names[c]}: {same}/{n} poses the same net point, max |d| {worst:.3f}; differing matrix elements by column {dict(sorted(col.items()))}')
