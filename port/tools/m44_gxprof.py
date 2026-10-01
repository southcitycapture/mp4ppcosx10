#!/usr/bin/env python3
"""M44 (PLAN.md 59): the GX front end inside a `sample` call graph.

    python3 port/tools/m44_gxprof.py FILE [--top 40]

The front end is entered wherever the game calls a GX function (a frame
named GX...) -- the port's GXCallDisplayList, GXBegin/GXEnd, the state
setters and the matrix loads (whose touch runs the lazy flush's apply).
For the game thread: the samples with such an entry on the stack (the
outermost one counts), by entry point; and the self samples of every
function under them, i.e. where inside the front end the time goes.
Also: the same share of the thread and of its busy samples (the thread less
the retrace wait)."""
import re
import sys
from collections import defaultdict

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from sample_tree import parse  # noqa: E402

IDLE = ("VIWaitForRetrace",)


def is_entry(name):
    return re.match(r"^GX[A-Z]", name) is not None


def main():
    path = sys.argv[1]
    top = 40
    if "--top" in sys.argv:
        top = int(sys.argv[sys.argv.index("--top") + 1])
    threads = parse(path)
    k = next(i for i, (_, nodes) in enumerate(threads) if any(n == "run_game" for (_, _, n) in nodes))
    name, nodes = threads[k]
    total = nodes[0][1]
    # self per node
    selfc = []
    stack = []
    for i, (d, c, n) in enumerate(nodes):
        while stack and nodes[stack[-1]][0] >= d:
            stack.pop()
        if stack:
            selfc[stack[-1]] -= c
        selfc.append(c)
        stack.append(i)
    entry_incl = defaultdict(int)
    fe_self = defaultdict(int)
    idle = 0
    fe_total = 0
    stack = []
    for i, (d, c, n) in enumerate(nodes):
        while stack and nodes[stack[-1]][0] >= d:
            stack.pop()
        above = [nodes[j][2] for j in stack]
        in_fe = any(is_entry(a) for a in above)
        if n in IDLE and not any(a in IDLE for a in above):
            idle += c
        if is_entry(n) and not in_fe:
            entry_incl[n] += c
            fe_total += c
        if in_fe or is_entry(n):
            fe_self[n] += selfc[i]
        stack.append(i)
    busy = total - idle
    print("%s: %d samples, %d busy (less %s), front end %d = %.1f%% of the thread, %.1f%% of busy"
          % (name, total, busy, IDLE[0], fe_total, 100.0 * fe_total / total, 100.0 * fe_total / busy))
    print("\nentry points (outermost GX* frame), inclusive:")
    for s, v in sorted(entry_incl.items(), key=lambda kv: -kv[1])[:top]:
        print("  %-44s %6d %5.1f%%" % (s[:44], v, 100.0 * v / fe_total))
    print("\nself inside the front end:")
    for s, v in sorted(fe_self.items(), key=lambda kv: -kv[1])[:top]:
        print("  %-44s %6d %5.1f%%" % (s[:44], v, 100.0 * v / fe_total))


if __name__ == "__main__":
    main()
