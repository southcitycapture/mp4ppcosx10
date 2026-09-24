#!/usr/bin/env python3
"""M42: symbolize a `sample` call graph whose process exited before sample
could read its symbols (the report then holds bare addresses).  Main-binary
addresses are resolved from the linker map (build-ppc-darwin/marioparty4.map,
the SAME build as the sampled binary); others become `lib@0x...` (system) or
`rel@0x...` (a REL module bundle).

    m42_symbolize.py SAMPLE.txt MAP > SAMPLE.sym.txt"""
import bisect, re, sys
src, mp = sys.argv[1], sys.argv[2]
addrs, names = [], []
on = False
for line in open(mp, errors='replace'):
    if line.startswith('# Symbols'):
        on = True
        continue
    if not on or not line.startswith('0x'):
        continue
    p = line.split('\t')
    if len(p) < 4:
        continue
    a = int(p[0], 16); n = p[3].strip()
    if n.startswith('_'):
        n = n[1:]
    addrs.append(a); names.append(n)
order = sorted(range(len(addrs)), key=lambda i: addrs[i])
addrs = [addrs[i] for i in order]; names = [names[i] for i in order]
hi = addrs[-1] + 0x10000
def sym(m):
    a = int(m.group(1), 16)
    if addrs[0] <= a < hi:
        i = bisect.bisect_right(addrs, a) - 1
        return names[i]
    return ('lib@' if a >= 0x90000000 else 'rel@') + m.group(1)
pat = re.compile(r'\b0x([0-9a-f]+)\b')
for line in open(src, errors='replace'):
    m = re.match(r'^(\s*\d+ )(0x[0-9a-f]+)(.*)$', line)
    if m:
        line = m.group(1) + pat.sub(sym, m.group(2)) + m.group(3) + '\n'
    sys.stdout.write(line)
