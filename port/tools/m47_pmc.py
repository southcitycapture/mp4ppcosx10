#!/usr/bin/env python3
"""M47 (PLAN.md 62): --pmc logs side by side.

    m47_pmc.py [--consumed] LOG [LOG...]

The per-region table of each log's drawn frames (or consumed, --consumed):
Mcycles a frame per region, one column per log, and the total; then the L3
data misses a frame the same way (set 1's sixth counter)."""
import re
import sys


def table(path, which):
    rows, on = {}, False
    for line in open(path, errors='replace'):
        if line.startswith('port> pmc: %s frames' % which):
            on = True
            continue
        if on:
            m = re.match(r'port> pmc:\s+(.+?)\s{2,}([\d.]+)\s+([\d.]+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)', line)
            if m:
                rows[m.group(1)] = (float(m.group(3)), int(m.group(8)), float(m.group(2)))
                if m.group(1) == '(total)':
                    break
            elif 'region' not in line:
                break
    return rows


def main():
    args = sys.argv[1:]
    which = 'drawn'
    if args and args[0] == '--consumed':
        which, args = 'consumed', args[1:]
    tabs = [table(a, which) for a in args]
    names = []
    for t in tabs:
        for k in t:
            if k not in names:
                names.append(k)
    short = [re.sub(r'.*K-', '', a).replace('.log', '')[:22] for a in args]
    print('| region (%s frame, M cycles) | ' % which + ' | '.join(short) + ' |')
    print('|---|' + '---:|' * len(args))
    for n in names:
        print('| %s | ' % n + ' | '.join('%.3f' % t[n][0] if n in t else '' for t in tabs) + ' |')
    print()
    print('| region (L3 data misses) | ' + ' | '.join(short) + ' |')
    print('|---|' + '---:|' * len(args))
    for n in names:
        print('| %s | ' % n + ' | '.join('%d' % t[n][1] if n in t else '' for t in tabs) + ' |')


main()
