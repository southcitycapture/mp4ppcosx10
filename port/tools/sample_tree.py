#!/usr/bin/env python3
"""Read a `sample` call graph (Mac OS X 10.5's `sample PID SECS -file F`) and
print, for one thread, the inclusive and self sample counts per symbol --
the §32.4 / §43.1 profile tables.

    python3 port/tools/sample_tree.py FILE [--thread N] [--top 40] [--under SYM]

Inclusive: a symbol is counted once per sample however many times it recurs
on the stack (objCall -> objCall -> ... is one objCall).  Self: the node's
count minus its children's.  --under SYM restricts both to the samples that
have SYM on the stack (e.g. --under FaceDraw.isra.0).  Threads are numbered
in file order; the game thread is the one with run_game under it.
"""
import re
import sys
from collections import defaultdict


def parse(path):
    threads = []
    cur = None
    started = False
    for line in open(path, errors="replace"):
        if line.startswith("Call graph:"):
            started = True
            continue
        if not started:
            continue
        if line.startswith("Total number in stack"):
            break
        m = re.match(r"^( *)(\d+) (.*)$", line.rstrip("\n"))
        if not m:
            continue
        depth = len(m.group(1)) // 2
        count = int(m.group(2))
        name = m.group(3).strip()
        if depth == 2:  # "    7147 Thread_2a03"
            cur = []
            threads.append((name, cur))
        if cur is not None:
            cur.append((depth, count, name))
    return threads


def analyse(nodes, under=None):
    incl = defaultdict(int)
    self_ = defaultdict(int)
    stack = []  # (depth, count, name, children_sum_holder)
    # first pass: self = count - sum(children)
    holders = []
    for i, (depth, count, name) in enumerate(nodes):
        while stack and stack[-1][0] >= depth:
            stack.pop()
        holder = [count, 0]
        holders.append(holder)
        if stack:
            stack[-1][3][1] += count
        stack.append((depth, count, name, holder))
    # second pass: inclusive without recursion double counting, optional filter
    stack = []
    for i, (depth, count, name) in enumerate(nodes):
        while stack and stack[-1][0] >= depth:
            stack.pop()
        names_above = set(n for (_, _, n) in stack)
        if under is None or under in names_above or under == name:
            if name not in names_above:
                incl[name] += count
            holder = holders[i]
            self_[name] += holder[0] - holder[1]
        stack.append((depth, count, name))
    return incl, self_


def main():
    args = sys.argv[1:]
    path = args[0]
    thread = None
    top = 40
    under = None
    i = 1
    while i < len(args):
        if args[i] == "--thread":
            thread = int(args[i + 1]); i += 2
        elif args[i] == "--top":
            top = int(args[i + 1]); i += 2
        elif args[i] == "--under":
            under = args[i + 1]; i += 2
        else:
            i += 1
    threads = parse(path)
    if thread is None:
        for k, (name, nodes) in enumerate(threads):
            if any(n == "run_game" for (_, _, n) in nodes):
                thread = k
                break
        if thread is None:
            thread = 0
    name, nodes = threads[thread]
    total = nodes[0][1]
    incl, self_ = analyse(nodes, under)
    base = incl[under] if under else total
    print("%s: %d samples%s" % (name, total, (" (%d under %s)" % (base, under)) if under else ""))
    print("%-48s %8s %6s %8s %6s" % ("symbol", "incl", "%", "self", "%"))
    for sym, n in sorted(incl.items(), key=lambda kv: -kv[1])[:top]:
        print("%-48s %8d %5.1f%% %8d %5.1f%%" % (sym[:48], n, 100.0 * n / base, self_[sym], 100.0 * self_[sym] / base))
    print()
    print("by self:")
    for sym, n in sorted(self_.items(), key=lambda kv: -kv[1])[:top]:
        print("%-48s %8d %5.1f%%" % (sym[:48], n, 100.0 * n / base))


if __name__ == "__main__":
    main()
