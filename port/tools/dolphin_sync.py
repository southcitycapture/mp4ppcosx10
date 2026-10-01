#!/usr/bin/env python3
"""dolphin_sync.py -- find where each of a session recording's frames falls on
the console's timeline, by running Dolphin (M51, PLAN.md 66.3).

  dolphin_sync.py REC WORKDIR [--iters N] [--until R] [--speed]

The port runs one retrace per game frame and reads its disc image at once;
the console waits on its disc: the main loop stalls while VCounter runs on,
and the game's own waits (a load polled every frame, a logo timed on the
clock) last a different number of frames.  The game itself is the same --
the engine RNG, rand8's LCG, the board RNG (given the port's seeds by the C2
hook) and the overlays take the same values in the same order (PLAN.md
66.3) -- so the map is found by aligning the two traces:

  1. build the .dtm from the current map (rec2dtm.py) and run Dolphin with
     MemoryWatcher on GlobalCounter, VCounter, the overlay and the RNGs;
  2. walk the recording's events (a change of the overlay or an RNG) and
     find each in Dolphin's trace in order (Dolphin may show values mid-frame
     that the port's end-of-frame samples never see, never the reverse);
  3. every matched event pins the GlobalCounter offset D between the rigs
     there; between two events the port's input goes to the console's frame
     with GlobalCounter = the port's + D (the game consumes PADRead's reading
     at the first field of that frame; a stall field repeats it), and where D
     changes between two events -- a wait of a different length -- the switch
     is put after the last input change before the later event (the press
     that started the wait keeps its place, the held input spans the wait);
  4. the first event not found is where the input landed wrong: rebuild the
     map with everything matched so far, run again from the boot; stop when
     every event of the recording is found.

Writes WORKDIR/map.json (the vmap: retrace -> VCounter; the gc map), the final
.dtm and GMPE01.ini, and WORKDIR/sync.log.
"""
import argparse, csv, json, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from rec2dtm import parse_rec, sig_timeline, pad_timeline, build_dtm, write_gecko  # noqa: E402

WATCH = ['801D3CE0:ovl', '801D3D10:frand', '801D342C:rnd8', '801D3F14:brand']


def log(wd, s):
    line = time.strftime('%H:%M:%S ') + s
    print(line, flush=True)
    open(os.path.join(wd, 'sync.log'), 'a').write(line + '\n')


def run_dolphin(wd, tag, dtm, gecko, until_vc, timeout, extra=()):
    out = os.path.join(wd, tag)
    cmd = [sys.executable, os.path.join(HERE, 'dolphin_watch.py'), out, '--dtm', dtm, '--gecko', gecko,
           '--until-vc', str(until_vc), '--timeout', str(timeout)]
    for w in WATCH:
        cmd += ['--watch', w]
    cmd += list(extra)
    subprocess.run(cmd, check=False, stdout=subprocess.DEVNULL)
    return os.path.join(out, 'watch.csv')


def dolphin_trace(path):
    """per VCounter: (gc, ovl, frand, rnd8, brand), forward-filled"""
    st = {'gc': 0, 'ovl': 0, 'frand': 0, 'rnd8': 0xD9ED, 'brand': 0}
    at = {}
    vmax = 0
    for r in csv.DictReader(open(path)):
        st[r['name']] = int(r['value'], 16)
        v = int(r['vc'])
        at[v] = (st['gc'], st['ovl'], st['frand'], st['rnd8'], st['brand'])
        vmax = max(vmax, v)
    out = []
    cur = (0, 0, 0, 0xD9ED, 0)
    for v in range(vmax + 1):
        if v in at:
            cur = at[v]
        out.append(cur)
    return out


def events(seq):
    """(index, key) where key = (ovl, frand, rnd8, brand) changes; zero RNGs skipped"""
    out = []
    last = None
    for k, s in enumerate(seq):
        if s is None:
            continue
        key = s[1:]
        if key != last:
            if key[1] != 0:
                out.append((k, key))
            last = key
    return out


def match(pe, de, look=400):
    pairs = []
    j = 0
    for (r, key) in pe:
        jj = j
        while jj < len(de) and jj < j + look and de[jj][1] != key:
            jj += 1
        if jj < len(de) and de[jj][1] == key:
            pairs.append((r, de[jj][0]))
            j = jj + 1
        else:
            return pairs, (r, key)
    return pairs, None


def build_vmap(rec, ps, dtr, pairs, fail_r, strategy):
    """the port retrace -> the console's VCounter"""
    end = rec['end']
    st = pad_timeline(rec)
    gp = [s[0] if s else 0 for s in ps]
    vmap = [None] * (end + 1)
    if dtr:
        gd = [t[0] for t in dtr]
        # first field of each Dolphin GlobalCounter value
        first_v = {}
        for v, g in enumerate(gd):
            if g not in first_v:
                first_v[g] = v
        gmax = gd[-1]
    else:
        first_v, gmax = {}, -1
    # the gc offset D at each matched pair
    anchors = [(r, (dtr[v][0] if dtr else 0) - gp[r]) for (r, v) in pairs] if pairs else []
    if not anchors:
        anchors = [(0, -4)]
    D = [None] * (end + 1)
    for i, (ra, da) in enumerate(anchors):
        rb = anchors[i + 1][0] if i + 1 < len(anchors) else end + 1
        db = anchors[i + 1][1] if i + 1 < len(anchors) else da
        sw = rb
        if db != da:
            # the switch: after the last input change in (ra, rb)
            last_change = ra
            for r in range(ra + 1, rb):
                if st[r] != st[r - 1]:
                    last_change = r
            sw = last_change + 1 if strategy == 'after-last-input' else ra + 1
            sw = min(max(sw, ra + 1), rb)
        for r in range(ra, rb):
            D[r] = da if r < sw else db
    for r in range(anchors[0][0]):
        D[r] = anchors[0][1]
    # map: the first field whose GlobalCounter is the port's + D; past Dolphin's
    # trace, one field a frame from the last placed retrace
    last_r, last_v = None, None
    for r in range(end + 1):
        g = gp[r] + D[r]
        v = first_v.get(g) if g <= gmax else None
        if v is None:
            if last_v is None:
                v = r + D[r]
            else:
                v = last_v + (r - last_r)
        if last_v is not None and v <= last_v:
            v = last_v  # two retraces into one field: the later one wins
        vmap[r] = v
        last_r, last_v = r, v
    return vmap


def first_input_change(st, lo, hi):
    for r in range(max(lo, 1), hi + 1):
        if st[r] != st[r - 1]:
            return r
    return None


def place(rec, ps, dtr, pairs, base_vmap, r_from, shift):
    """the map: base_vmap below r_from, the trace's GlobalCounter offsets from it"""
    new = build_vmap(rec, ps, dtr, pairs, None, 'after-last-input')
    vmap = list(base_vmap) if base_vmap else list(new)
    for r in range(r_from, rec['end'] + 1):
        vmap[r] = new[r] + shift
    for r in range(max(r_from, 1), rec['end'] + 1):
        if vmap[r] < vmap[r - 1]:
            vmap[r] = vmap[r - 1]
    return vmap


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rec')
    ap.add_argument('wd')
    ap.add_argument('--iters', type=int, default=60)
    ap.add_argument('--until', type=int, help='sync only up to this retrace')
    ap.add_argument('--timeout', type=int, default=3600)
    ap.add_argument('--margin', type=int, default=600, help='fields past the first unmatched event')
    ap.add_argument('--window', type=int, default=90, help='frames before a divergence whose input is re-placed')
    ap.add_argument('--start-trace', help='a watch.csv of an earlier run of this recording to start from')
    a = ap.parse_args()
    os.makedirs(a.wd, exist_ok=True)
    rec = parse_rec(a.rec)
    if a.until:
        rec['end'] = min(rec['end'], a.until)
    ps = sig_timeline(rec)[:rec['end'] + 1]
    st = pad_timeline(rec)
    pe = events(ps)
    gecko = os.path.join(a.wd, 'GMPE01.ini')
    write_gecko(rec, gecko)
    log(a.wd, f'{a.rec}: {rec["end"]} retraces, {len(pe)} events, {len(rec["boards"])} board seeds')
    # the first map: the start trace's, or one field a retrace
    if a.start_trace:
        dtr0 = dolphin_trace(a.start_trace)
        p0, f0 = match(pe, events(dtr0))
        vmap = place(rec, ps, dtr0, p0, None, 0, 0)
        log(a.wd, f'start trace {a.start_trace}: {len(p0)} events matched')
    else:
        vmap = [r - 4 for r in range(rec['end'] + 1)]
    frozen_body, frozen_v = None, 0
    best = None           # (fail_r, vmap, body, trace, pairs)
    shifts = [0, -1, 1, -2, 2, -3, 3]
    tries = 0
    window = a.window
    for it in range(a.iters):
        dtm = os.path.join(a.wd, f'it{it:02d}.dtm')
        info = build_dtm(rec, vmap, dtm, prefix=frozen_body, prefix_fields=frozen_v)
        body = info['body']
        horizon = vmap[rec['end']] + 120
        if best is not None:
            horizon = min(horizon, vmap[min(best[0] + 3000, rec['end'])] + a.margin)
        t0 = time.time()
        path = run_dolphin(a.wd, f'it{it:02d}', dtm, gecko, horizon, a.timeout)
        dtr = dolphin_trace(path)
        pairs, fail = match(pe, events(dtr))
        fail_r = fail[0] if fail else rec['end'] + 1
        log(a.wd, f'it {it}: to field {len(dtr) - 1} in {time.time() - t0:.0f} s; {len(pairs)} of {len(pe)} '
                  f'events; first unmatched {fail[0] if fail else "none"} (frozen to field {frozen_v}, '
                  f'shift {shifts[tries % len(shifts)] if best else 0}, window {window})')
        json.dump({'vmap': {str(r): v for r, v in enumerate(vmap)}, 'pairs': pairs, 'fail': fail,
                   'iteration': it}, open(os.path.join(a.wd, f'it{it:02d}.map.json'), 'w'))
        if not fail:
            os.replace(dtm, os.path.join(a.wd, 'session.dtm'))
            json.dump({'vmap': {str(r): v for r, v in enumerate(vmap)}, 'pairs': pairs, 'iteration': it,
                       'gc_d': [t[0] for t in dtr]}, open(os.path.join(a.wd, 'map.json'), 'w'))
            log(a.wd, f'IN STEP: every event of the recording found in Dolphin\'s run (it {it}): '
                      f'{os.path.join(a.wd, "session.dtm")}')
            return 0
        if best is None or fail_r > best[0]:
            best = (fail_r, vmap, body, dtr, pairs)
            tries = 0
            window = a.window
        else:
            tries += 1
            if tries >= len(shifts):
                tries = 0
                window *= 2
                if window > 2000:
                    log(a.wd, f'STUCK at retrace {best[0]}: every shift of the input before it tried -- the '
                              f'game itself differs there (PLAN.md 66.3)')
                    return 2
        bfail, bvmap, bbody, btr, bpairs = best
        r_c = first_input_change(st, bfail - window, bfail) or max(1, bfail - window)
        frozen_v = max(0, bvmap[r_c] - 4)
        frozen_body = bbody
        vmap = place(rec, ps, btr, bpairs, bvmap, r_c, shifts[tries % len(shifts)])
    return 1


if __name__ == '__main__':
    sys.exit(main())
