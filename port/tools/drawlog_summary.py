#!/usr/bin/env python3
"""One line per draw of a --drawlog: draw, prim, verts, object, loader, viewport,
the stages (colour/alpha inputs, dest register, texmap size/gl name), the konst,
the raster state.  M30 (PLAN.md 45): reading a frame's 500 draws in a screen.

    drawlog_summary.py LOG [--grep PATTERN]
"""
import re, sys

def main():
    path = sys.argv[1]
    pat = None
    if len(sys.argv) > 3 and sys.argv[2] == '--grep':
        pat = re.compile(sys.argv[3])
    cur = None
    out = []
    def flush():
        if cur is None:
            return
        st = ' | '.join(cur['stages'])
        line = ("d%-4s p%s v%-5s %-28s %-22s vp %-18s %s  reg %s  chan %s  a%s z%s b%s c%s" % (
            cur['n'], cur['prim'], cur['verts'], cur['obj'][:28], cur['by'][:22], cur['vp'],
            st, cur['reg'], cur['chan'], cur['alpha'], cur['z'], cur['blend'], cur['cull']))
        if pat is None or pat.search(line):
            out.append(line)
    for raw in open(path, errors='replace'):
        m = re.match(r'---- draw (\d+): prim (\w+), (\d+) verts', raw)
        if m:
            flush()
            cur = dict(n=m.group(1), prim=m.group(2), verts=m.group(3), obj='?', by='?', vp='?',
                       stages=[], reg='', chan='', alpha='', z='', blend='', cull='')
            continue
        if raw.startswith('port> copy:') or raw.startswith('gxwarn>'):
            flush(); cur = None
            if pat is None or pat.search(raw):
                out.append(raw.rstrip())
            continue
        if cur is None:
            continue
        m = re.search(r'drawobj model (\d+) object "([^"]*)"', raw)
        if m:
            cur['obj'] = 'm%s:%s' % (m.group(1), m.group(2)); continue
        m = re.search(r'loaded by (\S+)', raw)
        if m:
            cur['by'] = m.group(1); continue
        m = re.search(r'viewport (\S+ \S+ \S+ \S+)', raw)
        if m:
            cur['vp'] = m.group(1); continue
        m = re.match(r'\s+stage(\d) coord (\d+) map (\d+) chan (\d+)\s+cin (\d+ \d+ \d+ \d+)\s+ain (\d+ \d+ \d+ \d+).*creg (\d+) areg (\d+)\s+konst (\S+ \S+ \S+) / (\S+)', raw)
        if m:
            s = 's%s c[%s] a[%s]' % (m.group(1), m.group(5), m.group(6))
            if m.group(7) != '0' or m.group(8) != '0':
                s += ' ->R%s/R%s' % (m.group(7), m.group(8))
            s += ' k(%s/%s)' % (m.group(9), m.group(10))
            if m.group(4) != '4':
                s += ' ch%s' % m.group(4)
            cur['stages'].append(s); continue
        m = re.match(r'\s+texmap(\d+) (\d+x\d+) fmt (\d+) ci (\d) tlut (\d+) gl (\d+)', raw)
        if m:
            cur['stages'][-1] += ' T%s=%s/f%s/gl%s' % (m.group(1), m.group(2), m.group(3), m.group(6)); continue
        if 'NOT BOUND' in raw and cur['stages']:
            cur['stages'][-1] += ' T-'; continue
        m = re.match(r'\s+tevreg prev\s+(\S+\s+\S+\s+\S+\s+\S+)\s+c0\s+(\S+\s+\S+\s+\S+\s+\S+)\s+c1\s+(\S+\s+\S+\s+\S+\s+\S+)\s+c2\s+(\S+\s+\S+\s+\S+\s+\S+)', raw)
        if m:
            cur['reg'] = 'c0(%s) c1(%s) c2(%s)' % tuple(re.sub(r'\s+', ',', g.strip()) for g in m.groups()[1:]); continue
        m = re.match(r'\s+chan0 enable (\d) matsrc (\d) mat (\d+ \d+ \d+ \d+)', raw)
        if m:
            cur['chan'] = 'en%s ms%s mat(%s)' % (m.group(1), m.group(2), re.sub(r'\s+', ',', m.group(3))); continue
        m = re.match(r'\s+alphacmp (\d+) ref (\d+) op (\d+) / (\d+) ref (\d+)\s+zmode test (\d) fn (\d) write (\d)', raw)
        if m:
            cur['alpha'] = '%s/%s' % (m.group(1), m.group(2)); cur['z'] = '%s/%s/w%s' % (m.group(6), m.group(7), m.group(8)); continue
        m = re.match(r'\s+blend mode (\d) src (\d) dst (\d)\s+cull (\d)\s+scissor (\S+ \S+ \S+ \S+)', raw)
        if m:
            cur['blend'] = '%s/%s/%s' % (m.group(1), m.group(2), m.group(3)); cur['cull'] = m.group(4)
            if m.group(5) != '0 0 640 480':
                cur['vp'] += ' sc ' + m.group(5)
            continue
    flush()
    print('\n'.join(out))

if __name__ == '__main__':
    main()
