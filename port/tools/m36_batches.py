#!/usr/bin/env python3
"""M36 (PLAN.md 51.6): the draw calls of one drawn frame and how many of them
could be one call -- runs of consecutive draws that share every texture and
every piece of raster state (the sprite-batching question of PLAN.md 37.2).

    m36_batches.py LOG [LOG...]      a --drawlog N --drawlog-at F log per scene

Two keys per draw:
  * `state`  = prim, the stages (inputs, konst, texmaps' gl names), the
               channel, alpha test, z mode, blend, cull, scissor, projection,
               viewport -- everything the GL state machine would have to be
               set to before the call;
  * `state+mtx` = the above plus the position matrix, i.e. what a batch
               needs if the vertices are NOT re-transformed on the CPU.
A run is a maximal sequence of consecutive draws with an equal key.  The
table gives draws, runs, the draws that sit in a run of 2+ (the batchable
share), the largest run, and the vertices per draw.
"""
import re, sys, collections

def parse(path):
    draws = []
    cur = None
    for raw in open(path, errors='replace'):
        m = re.match(r'---- draw (\d+): prim (\w+), (\d+) verts', raw)
        if m:
            if cur:
                draws.append(cur)
            cur = dict(n=int(m.group(1)), prim=m.group(2), verts=int(m.group(3)),
                       stages=[], tex=[], obj='', pos='', proj='', vp='', chan='', alpha='',
                       z='', blend='', cull='', sc='', frame=None)
            continue
        if raw.startswith('port> ') and cur and not raw.startswith('port> copy'):
            # the drawlog's frame is bounded by the port's own lines
            pass
        if cur is None:
            continue
        m = re.match(r"\s+posmtx(\d+)\s+(.*)", raw)
        if m:
            cur['pos'] = m.group(2); continue
        m = re.search(r'proj (\S+) \[([^\]]*)\]\s+viewport (\S+ \S+ \S+ \S+)', raw)
        if m:
            cur['proj'] = m.group(1) + m.group(2); cur['vp'] = m.group(3); continue
        m = re.match(r'\s+stage(\d) coord (\d+) map (\d+) chan (\d+)\s+cin (\d+ \d+ \d+ \d+)\s+ain (\d+ \d+ \d+ \d+).*creg (\d+) areg (\d+)\s+konst (\S+ \S+ \S+) / (\S+)', raw)
        if m:
            cur['stages'].append(m.group(0).strip()); continue
        m = re.match(r'\s+texmap(\d+) (\d+x\d+) fmt (\d+) ci (\d) tlut (\d+) gl (\d+)', raw)
        if m:
            cur['tex'].append('gl%s' % m.group(6)); continue
        if 'NOT BOUND' in raw:
            cur['tex'].append('gl-'); continue
        m = re.match(r'\s+chan0 enable (\d) matsrc (\d) mat (\d+ \d+ \d+ \d+)', raw)
        if m:
            cur['chan'] = m.group(0).strip(); continue
        m = re.match(r'\s+alphacmp (\d+) ref (\d+) op (\d+) / (\d+) ref (\d+)\s+zmode test (\d) fn (\d) write (\d)', raw)
        if m:
            cur['alpha'] = m.group(0).strip(); continue
        m = re.match(r'\s+blend mode (\d) src (\d) dst (\d)\s+cull (\d)\s+scissor (\S+ \S+ \S+ \S+)', raw)
        if m:
            cur['blend'] = m.group(0).strip(); continue
        m = re.search(r'drawobj model (\d+) object "([^"]*)"', raw)
        if m:
            cur['obj'] = m.group(2); continue
    if cur:
        draws.append(cur)
    return draws

def key(d, with_mtx):
    k = (d['prim'], tuple(d['stages']), tuple(d['tex']), d['chan'], d['alpha'], d['blend'],
         d['proj'], d['vp'])
    if with_mtx:
        k = k + (d['pos'],)
    return k

def runs(draws, with_mtx):
    out = []
    i = 0
    while i < len(draws):
        j = i + 1
        k = key(draws[i], with_mtx)
        while j < len(draws) and key(draws[j], with_mtx) == k:
            j += 1
        out.append((i, j - i))
        i = j
    return out

def report(path):
    draws = parse(path)
    if not draws:
        print("%s: no draws" % path)
        return
    n = len(draws)
    verts = sum(d['verts'] for d in draws)
    quads = sum(1 for d in draws if d['prim'] in ('80',))
    rows = []
    for with_mtx in (False, True):
        r = runs(draws, with_mtx)
        in_runs = sum(l for _, l in r if l >= 2)
        largest = max(l for _, l in r)
        rows.append((('state+mtx' if with_mtx else 'state'), len(r), in_runs, largest))
    tex = collections.Counter(tuple(d['tex']) for d in draws)
    print("%s: %d draws, %d vertices (%.1f a draw), %d quad draws (prim 80), %d distinct texture sets"
          % (path, n, verts, verts / n, quads, len(tex)))
    for name, nr, in_runs, largest in rows:
        print("   key %-10s  runs %4d  draws in runs of 2+: %4d (%3.0f%%)  largest run %3d  calls if batched %4d"
              % (name, nr, in_runs, 100.0 * in_runs / n, largest, nr))
    # the ten longest same-state runs, named
    r = sorted(runs(draws, False), key=lambda x: -x[1])[:8]
    for i, l in r:
        d = draws[i]
        print("     run of %3d from draw %4d: prim %s %s tex %s obj '%s'"
              % (l, d['n'], d['prim'], d['proj'][:5], ','.join(d['tex']), d['obj'][:24]))

for p in sys.argv[1:]:
    report(p)
