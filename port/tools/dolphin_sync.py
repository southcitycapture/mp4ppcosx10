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
from rec2dtm import parse_rec, sig_timeline, pad_timeline, build_dtm, write_gecko, gecko_resync  # noqa: E402

WATCH = ['801D3CE0:ovl', '801D3D10:frand', '801D342C:rnd8', '801D3F14:brand'] + \
        [f'{0x801923C0 + i * 0x180 + 0x80:08X}:win{i}' for i in range(6)]  # winData[i].num_chars/max_chars


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
    names = ['gc', 'ovl', 'frand', 'rnd8', 'brand'] + [f'win{i}' for i in range(6)]
    st = {n: 0 for n in names}
    st['rnd8'] = 0xD9ED
    at = {}
    vmax = 0
    for r in csv.DictReader(open(path)):
        st[r['name']] = int(r['value'], 16)
        v = int(r['vc'])
        at[v] = tuple(st[n] for n in names)
        vmax = max(vmax, v)
    out = []
    cur = tuple(st0 for st0 in [0, 0, 0, 0xD9ED, 0] + [0] * 6)
    for v in range(vmax + 1):
        if v in at:
            cur = at[v]
        out.append(cur)
    return out


def events(seq, part='hard'):
    """(index, key) where the key changes: 'hard' = (ovl, frand, rnd8), the
    game's own clock of events, which must match exactly; 'board' = the
    board RNG, whose values must come in the same order but may come a few
    frames apart (a COM's decision after a wait on the sound or the disc,
    PLAN.md 66.3); 'win' = the message
    windows' typing, a hint for the stretches where nothing random happens
    (Dolphin's sample of it can be a frame or two off the port's, PLAN.md
    66.3); zero frand (the boot) skipped"""
    out = []
    last = None
    for k, s in enumerate(seq):
        if s is None:
            continue
        key = s[1:4] if part == 'hard' else s[4:5] if part == 'board' else s[5:]
        if key != last:
            if s[2] != 0:
                out.append((k, key))
            last = key
    return out


def match_seq(pe, de, look=400, boot=100):
    pairs = []
    j = 0
    skipped = 0
    for i, (r, key) in enumerate(pe):
        jj = j
        while jj < len(de) and jj < j + look and de[jj][1] != key:
            jj += 1
        if jj < len(de) and de[jj][1] == key:
            pairs.append((r, de[jj][0]))
            j = jj + 1
            skipped = 0
        elif r < boot:
            continue  # the boot's first samples: Dolphin's watcher may start a field late
        elif skipped < 3 and any(pe[i + k][1] in [de[x][1] for x in range(j, min(len(de), j + look))]
                                 for k in (1, 2, 3) if i + k < len(pe)):
            # a state of the port's the console's samples do not show, and
            # the next ones in order: the watcher missed a frame's sample, or
            # the same random numbers were drawn a frame apart -- the engine
            # RNG's chain rejoins only if they were the same draws: not a
            # divergence (PLAN.md 66.3)
            skipped += 1
            continue
        else:
            return pairs, (r, key)
    return pairs, None


def match(pe, de, ps=None, dtr=None):
    """the hard events in order (the sync's verdict); with the traces, the
    windows' events matched inside every RNG-silent gap of 20+ frames between
    two matched hard events (and after the last), for the placement"""
    pairs, fail = match_seq(pe, de)
    if ps is not None and dtr is not None:
        # the board RNG: every value the port's took, in order, in Dolphin's
        # trace up to the matched point (+ a margin), extras allowed
        endr = fail[0] if fail else len(ps)
        pb = [e for e in events(ps, 'board') if e[0] < endr and e[1][0] != 0]
        vend = pairs[-1][1] + 600 if pairs else len(dtr)
        db = [e for e in events(dtr, 'board') if e[0] < vend and e[1][0] != 0]
        bp, bf = match_seq(pb, db, look=4000, boot=0)
        if bf and bf[0] < endr - 600:
            # a board value never taken: the game differs from there
            k = 0
            while k < len(pairs) and pairs[k][0] < bf[0]:
                k += 1
            pairs, fail = pairs[:k], (bf[0], ('board',) + bf[1])
    if ps is None or dtr is None or not pairs:
        return pairs, fail
    pw = events(ps, 'win')
    dw = events(dtr, 'win')
    extra = []
    bounds = pairs + [((fail[0] if fail else len(ps)), len(dtr))]
    for (ra, va), (rb, vb) in zip(bounds, bounds[1:]):
        if rb - ra < 20:
            continue
        sp, _ = match_seq([e for e in pw if ra < e[0] < rb], [e for e in dw if va < e[0] < vb], boot=0)
        extra += sp
    allp = sorted(set(pairs + extra))
    # keep it monotonic in both
    out = []
    for r, v in allp:
        if out and v < out[-1][1]:
            continue
        out.append((r, v))
    return out, fail


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
            # GlobalCounter does not move in a disc stall, so a change of D
            # between two events is a wait of the game's own -- most often for
            # this very input: it keeps the earlier offset ('before-next');
            # the other placements are the retries
            sw = (rb if strategy == 'before-next' else last_change + 1 if strategy == 'after-last-input'
                  else ra + 1)
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


def field_of(pairs, r):
    """the field Dolphin's game was at for the port's retrace r, from the matched events"""
    best = None
    for (rp, vp) in pairs:
        if rp <= r:
            best = (rp, vp)
        else:
            break
    return best[1] + (r - best[0]) if best else r


def place(rec, ps, dtr, pairs, base_vmap, r_from, shift):
    """the map: base_vmap below r_from; from it, by the game's state: the
    matched events say on which field Dolphin's game was in the state the
    port's was in before PADRead r (its `s` line), and Dolphin's watcher
    samples a field after the frame that read that field's input has run --
    so the input of r goes on the field whose state is the port's at r + 1
    (PLAN.md 66.3).  Between events, one field a frame from the last; past
    the last matched event, the same."""
    vmap = list(base_vmap) if base_vmap else [r for r in range(rec['end'] + 1)]
    for r in range(r_from, rec['end'] + 1):
        vmap[r] = field_of(pairs, r + 1) + shift
    for r in range(max(r_from, 1), rec['end'] + 1):
        if vmap[r] < vmap[r - 1]:
            vmap[r] = vmap[r - 1]
    return vmap


def match_after(pe, dtr, ps, resync):
    if not resync:
        return match(pe, events(dtr), ps, dtr)
    r_res, v_res = resync
    ps2 = [None] * r_res + list(ps[r_res:])
    dtr2 = [None] * v_res + list(dtr[v_res:])
    return match(pe, [e for e in events(dtr2)], ps2, dtr2)


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
    ap.add_argument('--mode', default='global', choices=['global', 'window'],
                    help='global: every input re-placed after each run; window: only those before the divergence')
    ap.add_argument('--first-horizon', type=int, default=0, help='fields the first run goes to (0: the end)')
    ap.add_argument('--base-dtm', help='re-sync mode: the movie that takes Dolphin to its own instruction card')
    ap.add_argument('--base-trace', help="re-sync mode: that movie's watch.csv")
    ap.add_argument('--resync-delay', type=int, default=30, help='frames into the card where the state is handed over')
    ap.add_argument('--resume', help='an earlier iteration (WORKDIR/itNN): its movie, map and trace are the best so far')
    a = ap.parse_args()
    os.makedirs(a.wd, exist_ok=True)
    rec = parse_rec(a.rec)
    if rec['pokes']:
        sys.exit('dolphin_sync: the recording has the self-play harness\'s writes (a marathon or a teleport): '
                 'only sessions played from the boot replay in Dolphin (PLAN.md 66.3)')
    if a.until:
        rec['end'] = min(rec['end'], a.until)
    ps = sig_timeline(rec)[:rec['end'] + 1]
    st = pad_timeline(rec)
    pe = events(ps)
    gecko = os.path.join(a.wd, 'GMPE01.ini')
    resync = None
    if a.base_dtm:
        # PLAN.md 66.3: the console's board turns took other times (its computer
        # players act on the clock of waits the port does not have), so the
        # replay hands Dolphin the port's state at the minigame's instruction
        # card and aligns from there
        dtr0 = dolphin_trace(a.base_trace)
        v_entry = next(v for v in range(3000, len(dtr0)) if dtr0[v][1] == 3)
        G = dtr0[v_entry][0] + a.resync_delay
        v_res = next(v for v in range(v_entry, len(dtr0)) if dtr0[v][0] >= G)
        r_entry = next(r for r in range(3000, len(ps)) if ps[r] and ps[r][1] == 3)
        r_res = r_entry + a.resync_delay
        sp = ps[r_res]
        areas, mgn = {}, None
        for (r, name, data) in rec['areas']:
            if r <= r_res:
                if name == 'mgNext':
                    mgn = int.from_bytes(data, 'big')
                else:
                    areas[name] = data
        block = gecko_resync(G, sp[2], sp[3], sp[4], sorted(areas.items()), mgn)
        write_gecko(rec, gecko, extra=[('M51 the hand-over at the instruction card', block)])
        resync = (r_res, v_res)
        log(a.wd, f're-sync: the port\'s card from {r_entry}, Dolphin\'s from field {v_entry} (GlobalCounter '
                  f'{dtr0[v_entry][0]}); the state handed over at GlobalCounter {G} (field {v_res}) = the port\'s '
                  f'retrace {r_res}: frand {sp[2]:08x} rnd8 {sp[3]:08x} brand {sp[4]:08x}, '
                  f'{len(areas)} work areas{", mgNext %d" % mgn if mgn is not None else ""}')
        pe = [e for e in pe if e[0] >= r_res]
    else:
        write_gecko(rec, gecko)
    log(a.wd, f'{a.rec}: {rec["end"]} retraces, {len(pe)} events, {len(rec["boards"])} board seeds')
    # the first map: the start trace's, or one field a retrace
    if a.start_trace:
        dtr0 = dolphin_trace(a.start_trace)
        p0, f0 = match(pe, events(dtr0), ps, dtr0)
        vmap = place(rec, ps, dtr0, p0, None, 0, 0)
        log(a.wd, f'start trace {a.start_trace}: {len(p0)} events matched')
    else:
        vmap = [r - 4 for r in range(rec['end'] + 1)]
    frozen_body, frozen_v = None, 0
    if resync:
        r_res, v_res = resync
        vmap = [v_res - 1 if r < r_res else v_res + (r - r_res) for r in range(rec['end'] + 1)]
        frozen_body = open(a.base_dtm, 'rb').read()[256:]
        frozen_v = v_res - 2
    best = None           # (fail_r, vmap, body, trace, pairs)
    stuck = 0
    stale = False
    if a.resume:
        m = json.load(open(a.resume + '.map.json'))
        rv = [None] * (rec['end'] + 1)
        for k, v in m['vmap'].items():
            if int(k) <= rec['end']:
                rv[int(k)] = v
        rbody = open(a.resume + '.dtm', 'rb').read()[256:]
        rtr = dolphin_trace(os.path.join(a.resume, 'watch.csv'))
        rp, rf = match_after(pe, rtr, ps, resync)
        best = ((rf[0] if rf else rec['end'] + 1), rv, rbody, rtr, rp)
        log(a.wd, f'resume {a.resume}: {len(rp)} events matched, first unmatched {rf[0] if rf else "none"}')
        bf_eff = best[0]
        if rp and best[0] - rp[-1][0] > 60:
            bf_eff = rp[-1][0] + 30
        if a.mode == 'global':
            vmap = place(rec, ps, rtr, rp, None, 0, 0)
        else:
            r_c = first_input_change(st, bf_eff - a.window, bf_eff) or max(1, bf_eff - a.window)
            frozen_v = max(0, field_of(rp, r_c) - 4)
            frozen_body = rbody
            vmap = place(rec, ps, rtr, rp, rv, r_c, 0)
    shifts = [0, -1, 1, -2, 2, -3, 3]
    tries = 0
    window = a.window
    for it in range(a.iters):
        dtm = os.path.join(a.wd, f'it{it:02d}.dtm')
        info = build_dtm(rec, vmap, dtm, prefix=frozen_body, prefix_fields=frozen_v)
        body = info['body']
        horizon = vmap[rec['end']] + 120
        if best is None and a.first_horizon:
            horizon = min(horizon, a.first_horizon)
        if best is not None:
            horizon = min(horizon, vmap[min(best[0] + 3000, rec['end'])] + a.margin)
        t0 = time.time()
        path = run_dolphin(a.wd, f'it{it:02d}', dtm, gecko, horizon, a.timeout)
        dtr = dolphin_trace(path)
        pairs, fail = match_after(pe, dtr, ps, resync)
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
        stale = False
        if best is None or fail_r > best[0]:
            best = (fail_r, vmap, body, dtr, pairs)
            tries = 0
            window = a.window
            stuck = 0
        elif a.mode == 'global':
            stuck += 1
            stale = fail_r < best[0]
            if stuck >= 4:
                log(a.wd, f'STUCK at retrace {best[0]}: four maps from the runs\' own matches and no further '
                          f'-- the game itself differs there (PLAN.md 66.3)')
                return 2
        else:
            tries += 1
            if tries >= len(shifts):
                tries = 0
                window *= 2
                if window > 2000:
                    log(a.wd, f'STUCK at retrace {best[0]}: every shift of the input before it tried -- the '
                              f'game itself differs there (PLAN.md 66.3)')
                    return 2
        if a.mode == 'global':
            # every input re-placed from the latest run's own state matches:
            # a map that agrees with the trace it produces is in step, and
            # the matched start does not move once it is right (PLAN.md 66.3)
            frozen_v, frozen_body = 0, None
            src = (dtr, pairs) if not stale else (best[3], best[4])
            vmap = place(rec, ps, src[0], src[1], None, 0, 0)
            continue
        bfail, bvmap, bbody, btr, bpairs = best
        # a long quiet stretch before the first miss (nothing random moving --
        # a minigame's ending): what decided it came before the stretch
        if bpairs and bfail - bpairs[-1][0] > 60:
            bfail = bpairs[-1][0] + 30
        r_c = first_input_change(st, bfail - window, bfail) or max(1, bfail - window)
        frozen_v = max(0, field_of(bpairs, r_c) - 4)
        frozen_body = bbody
        vmap = place(rec, ps, btr, bpairs, bvmap, r_c, shifts[tries % len(shifts)])
    return 1


if __name__ == '__main__':
    sys.exit(main())
