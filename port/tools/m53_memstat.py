#!/usr/bin/env python3
# M53 (PLAN.md 68.4): the "port> mem" lines of a --memstat run as a table, and
# the status line's rss/res per hour of game -- m53_memstat.py LOG[.gz]
import gzip, re, sys
p = sys.argv[1]
f = gzip.open(p, 'rt', errors='replace') if p.endswith('.gz') else open(p, errors='replace')
print('| t (min) | frame | screen | rss MB | vsize MB | malloc in use / allocated MB | resident MB (files) | tex KB (entries) | vc region peak KB, entries / arrays | skin | modules |')
print('|---:|---:|---|---:|---:|---|---|---|---|---:|---:|')
for l in f:
    m = re.match(r'port> mem f(\d+)\s+(\S+)\s+t=(\d+)s\s+rss (\d+) MB\s+vsize (\d+) MB\s+malloc (\d+)/(\d+) KB\s+'
                 r'res (\d+) KB \((\d+) files\)\s+tex (\d+) entries (\d+) KB\s+vc region (\d+)/(\d+) KB \(peak (\d+)\) '
                 r'ent (\d+) arr (\d+)\s+skin (\d+)\s+modules (\d+)', l)
    if not m:
        continue
    g = m.groups()
    print(f'| {int(g[2])/60:.0f} | {g[0]} | {g[1]} | {g[3]} | {g[4]} | {int(g[5])/1024:.1f} / {int(g[6])/1024:.1f} | '
          f'{int(g[7])/1024:.1f} ({g[8]}) | {g[10]} ({g[9]}) | {g[13]}, {g[14]} / {g[15]} | {g[16]} | {g[17]} |')
