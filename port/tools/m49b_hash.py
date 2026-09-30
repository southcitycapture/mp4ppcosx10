#!/usr/bin/env python3
"""M49b (PLAN.md 64b): two --gamehash logs (Lite off / on) compared line by
line: identical to the end, or the first frame whose chain differs (and
whether the RNG seeds had moved there).  m49b_hash.py OFF.log ON.log"""
import re, sys
rx = re.compile(r'^port> gamehash f(\d+) mg (-?\d+) frand (\w+) rand8 (\w+) frame (\w+) chain (\w+)')
def load(p):
    return [m.groups() for m in map(rx.match, open(p, errors='replace')) if m]
a, b = load(sys.argv[1]), load(sys.argv[2])
mg = [x for x in a if x[1] not in ('-1', '0')]
span = f'mg frames f{mg[0][0]}..f{mg[-1][0]}' if mg else 'no minigame lines'
if len(a) == len(b) and a == b:
    print(f'IDENTICAL {len(a)} lines to f{a[-1][0]} ({span}), last chain {a[-1][5]}')
    sys.exit(0)
for i, (x, y) in enumerate(zip(a, b)):
    if x != y:
        rng = 'RNG same' if x[2:4] == y[2:4] else 'RNG moved'
        print(f'SPLIT at f{x[0]} (mg {x[1]}; {rng}); lines {len(a)} / {len(b)}; last good f{a[i-1][0] if i else "-"}')
        sys.exit(1)
print(f'PREFIX-EQUAL but lengths differ {len(a)} / {len(b)}')
sys.exit(1)
