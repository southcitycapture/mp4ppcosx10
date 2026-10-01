#!/usr/bin/env python3
"""M47 (PLAN.md 62): self samples per function in one thread of a `sample` file.

    m47_sample.py FILE [THREAD_INDEX (0: the first, the game thread)] [N]

`sample`'s call tree gives every node its inclusive count; a node's self count
is its count less its children's.  Summed by function name over the thread."""
import collections
import gzip
import re
import sys

path = sys.argv[1]
want = int(sys.argv[2]) if len(sys.argv) > 2 else 0
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
op = gzip.open if path.endswith('.gz') else open
lines = op(path, 'rt', errors='replace').read().split('\n')
threads, cur = [], None
node = re.compile(r'^(\s*[+!:| ]*)(\d+) (\S+)')
for ln in lines:
    if re.match(r'^\s+\d+ Thread_', ln):
        cur = []
        threads.append((ln.strip(), cur))
        continue
    if ln.startswith('Total number in stack'):
        break
    if cur is not None:
        m = node.match(ln)
        if m:
            cur.append((len(m.group(1)), int(m.group(2)), m.group(3)))
name, nodes = threads[want]
selfc = collections.Counter()
total = 0
for i, (d, c, f) in enumerate(nodes):
    kids = 0
    for d2, c2, f2 in nodes[i + 1:]:
        if d2 <= d:
            break
        # a direct child: the first deeper indent after this node
        if d2 == min(x[0] for x in nodes[i + 1:i + 2]):
            pass
    # direct children are the nodes at the next indent level until we return to d
    j = i + 1
    child_d = None
    while j < len(nodes) and nodes[j][0] > d:
        if child_d is None:
            child_d = nodes[j][0]
        if nodes[j][0] == child_d:
            kids += nodes[j][1]
        j += 1
    selfc[f] += c - kids
    if d == min(x[0] for x in nodes[:1]):
        total = max(total, c)
print('%s: %d samples' % (name, sum(selfc.values())))
for f, c in selfc.most_common(top):
    print('%7d %5.1f%%  %s' % (c, 100.0 * c / max(1, sum(selfc.values())), f))
