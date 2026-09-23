#!/usr/bin/env python3
"""M39c (PLAN.md 54b.6): port/docs/fps-boards.md's numbers.

    m39c_fps.py RDIR PDIR [--entry 5108]

RDIR holds the realtime runs R1..R6.log (plain or .gz; R1 may be any w01dll
soak: the M39 leave-behind's, docs/soak/m39c-soak31-m39-leave.log.gz): the
presented fps over the board's own status lines (median / p10), the speed.
PDIR holds the perf runs P1..P6.log + P1..P6.csv (m39c_perf.sh): per drawn
frame in E+3000..E+3900 the game thread's ms (game_ms), the decode (dec_ms
on the render thread + gdec_ms on the game thread), the replay (rt_ms); the
GL calls and the vertices GL is handed per drawn frame from --gltrace's
forty armed presented frames (the exit report's GX totals count the
fast-forward's decode too, so they are not used).
Prints a markdown table.
"""
import sys, os, re, csv, gzip, statistics as st
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import m39c_read

NAMES = {1: "Toad's Midway Madness", 2: "Goomba's Greedy Gala", 3: "Shy Guy's Jungle Jam",
         4: "Boo's Haunted Bash", 5: "Koopa's Seaside Soirée", 6: "Bowser's Gnarly Party"}


def find(d, stem):
    for s in (stem, stem + '.gz'):
        if os.path.exists(os.path.join(d, s)):
            return os.path.join(d, s)
    return None


def perf(pdir, n, entry):
    log = find(pdir, f'P{n}.log'); cp = find(pdir, f'P{n}.csv')
    out = {}
    if cp:
        rows = [r for r in csv.DictReader(open(cp)) if entry + 3000 <= int(r['frame']) <= entry + 3900]
        dr = [r for r in rows if r['drawn'] == '1']
        if dr:
            out['drawn'] = len(dr); out['retraces'] = len(rows)
            f = lambda k: st.median(float(r[k]) for r in dr)
            out['game'] = f('game_ms'); out['dec'] = f('dec_ms'); out['gdec'] = f('gdec_ms'); out['rt'] = f('rt_ms')
            out['work'] = f('work_ms')
            out['pfps'] = 60.0 * len(dr) / len(rows)
    if log:
        gl = draws = multi = verts = 0
        for line in open(log, errors='replace'):
            if line.startswith('gltrace> '):
                gl += 1
                if line.startswith('gltrace> glDrawArrays'):
                    draws += 1
                    m = re.search(r' count (\d+)', line)
                    verts += int(m.group(1)) if m else 0
                elif line.startswith('gltrace> glMultiDrawArrays'):
                    multi += 1
                    m = re.search(r' total (\d+)', line)
                    verts += int(m.group(1)) if m else 0
        # the trace's window is the forty presented frames up to F (gl13.c's
        # gl13_trace_armed), and the vertices are the ones GL was handed
        out['gl'] = gl / 40.0; out['draws'] = (draws + multi) / 40.0; out['verts'] = verts / 40.0
    return out


if __name__ == '__main__':
    rdir, pdir = sys.argv[1], sys.argv[2]
    entry = int(sys.argv[sys.argv.index('--entry') + 1]) if '--entry' in sys.argv else 5108
    print('| board | turns | presented fps, median / p10 (the board, 5 turns, real time) | speed | '
          'drawn frame: game / decode (render + game thread) / replay ms | GL calls a drawn frame (draw calls) | '
          'vertices a drawn frame |')
    print('|---|---:|---:|---:|---:|---:|---:|')
    for n in range(1, 7):
        rp = find(rdir, f'R{n}.log')
        r = m39c_read.read(rp) if rp else None
        p = perf(pdir, n, entry)
        a = f"{r['fps_med']:.1f} / {r['fps_p10']:.1f}" if r and r['n'] else '-'
        s = f"{r['speed']:.1f}%" if r and r['n'] else '-'
        t = f"{r['turn']}" if r else '-'
        g = (f"{p['game']:.1f} / {p['dec']:.1f} + {p['gdec']:.1f} / {p['rt']:.1f}" if 'game' in p else '-')
        c = (f"{p['gl']:.0f} ({p['draws']:.0f})" if p.get('gl') else '-')
        v = (f"{p['verts']:,.0f}" if 'verts' in p else '-')
        print(f"| w0{n} {NAMES[n]} | {t} | {a} | {s} | {g} | {c} | {v} |")
