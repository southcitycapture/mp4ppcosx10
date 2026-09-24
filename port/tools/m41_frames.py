#!/usr/bin/env python3
"""M41 (PLAN.md 56): two scoreboard chains' dumped frames, run by run --
tools/fps_board.sh writes `NAME EXIT=.. md5 FRAME:MD5 ...` per run into its
index.txt.  Prints the runs whose frames differ and the count identical.

    m41_frames.py BEFORE/index.txt AFTER/index.txt"""
import re, sys


def read(p):
    out = {}
    for line in open(p, errors="replace"):
        m = re.match(r"^(\S+) EXIT=\S+ .*?md5((?: (?:frame-)?\d+:[0-9a-f]{8})*)", line)
        if m:
            out[m.group(1)] = dict(x.replace("frame-", "").split(":") for x in m.group(2).split())
    return out


a, b = read(sys.argv[1]), read(sys.argv[2])
same = diff = 0
for k in sorted(set(a) & set(b)):
    fa, fb = a[k], b[k]
    for fr in sorted(set(fa) & set(fb), key=int):
        if fa[fr] == fb[fr]:
            same += 1
        else:
            diff += 1
            print("%-22s frame %6s  %s -> %s" % (k, fr, fa[fr], fb[fr]))
    only = set(fa) ^ set(fb)
    if only:
        print("%-22s frames in one chain only: %s" % (k, ", ".join(sorted(only, key=int))))
print("%d frames identical, %d differ" % (same, diff))
