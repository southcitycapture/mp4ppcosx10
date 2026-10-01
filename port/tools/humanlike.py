#!/usr/bin/env python3
"""humanlike.py -- a --play script that plays like a person (M51, PLAN.md 66.3):
the main stick wandering (a new direction and strength every 12-45 frames,
sometimes let go), A, B and the occasional X/Y pressed for 3-12 frames at
uneven intervals, now and then A mashed -- deterministic from --seed, so the
same script is the same "person" every time.

  humanlike.py BASE.play FROM TO [--seed N] > OUT.play

Everything of BASE.play before FROM is kept (the walk to the board); from
FROM to TO the person plays."""
import random, sys

base, frm, to = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
seed = int(sys.argv[sys.argv.index('--seed') + 1]) if '--seed' in sys.argv else 51
rnd = random.Random(seed)
print(f'# humanlike.py {base} {frm} {to} --seed {seed}: the walk to frame {frm}, then a person')
for line in open(base):
    t = line.split()
    if len(t) >= 3 and t[0] == 'at' and int(t[1]) >= frm:
        continue
    sys.stdout.write(line)
f = frm
while f < to:
    n = rnd.randint(12, 45)
    if rnd.random() < 0.2:
        f += n  # let go of the stick
        continue
    x = rnd.randint(-100, 100)
    y = rnd.randint(-100, 100)
    print(f'at {f} {n} stick:{x}:{y}')
    f += n
f = frm
while f < to:
    f += rnd.randint(8, 70)
    r = rnd.random()
    if r < 0.15:
        for k in range(rnd.randint(4, 10)):  # mashing
            print(f'at {f} 3 A')
            f += rnd.randint(5, 8)
        continue
    b = 'A' if r < 0.65 else 'B' if r < 0.85 else rnd.choice(['X', 'Y'])
    print(f'at {f} {rnd.randint(3, 12)} {b}')
