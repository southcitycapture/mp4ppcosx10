#!/usr/bin/env python3
"""M43 (PLAN.md 58): a `sample` thread's SELF time by object file (the link
map's), to choose the files a compiler lever is worth trying on.  Symbols
not in the map (REL module code, libSystem, the driver) are grouped by the
`(in LIB)` sample names them with.

    python3 port/tools/m43_objs.py SAMPLE.txt [--thread 0] [--map MAP] [--top 30]
"""
import re, sys, os, argparse
from collections import defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sample_tree import parse
from m41_split import load_map


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--thread", type=int, default=0)
    ap.add_argument("--map", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build-ppc-darwin", "marioparty4.map"))
    ap.add_argument("--top", type=int, default=30)
    a = ap.parse_args()
    sym = load_map(a.map)
    tname, nodes = parse(a.file)[a.thread]
    total = nodes[0][1]
    selfc = [c for (_, c, _) in nodes]
    stack = []
    for i, (d, c, n) in enumerate(nodes):
        while stack and nodes[stack[-1]][0] >= d:
            stack.pop()
        if stack:
            selfc[stack[-1]] -= c
        stack.append(i)
    by = defaultdict(int)
    for i, (d, c, n) in enumerate(nodes):
        s = selfc[i]
        if s <= 0:
            continue
        name = re.sub(r"\s+\(in .*$", "", n.split("  (in ")[0].split(" + ")[0].strip())
        lib = re.search(r"\(in ([^)]*)\)", n)
        if name in sym:
            k = os.path.basename(sym[name] or "?")
        elif re.search(r"semaphore_wait|mach_msg_trap|__semwait|_pthread_cond_wait|nanosleep|mach_wait_until", name):
            k = "(wait)"
        else:
            k = "(%s)" % (lib.group(1) if lib else "?")
        by[k] += s
    print("%s: %d samples (%s)" % (tname, total, a.file))
    for k, c in sorted(by.items(), key=lambda x: -x[1])[: a.top]:
        print("  %-40s %6d  %5.1f%%" % (k, c, 100.0 * c / total))


if __name__ == "__main__":
    main()
