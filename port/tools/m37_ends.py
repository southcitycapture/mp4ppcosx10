#!/usr/bin/env python3
"""M37 (PLAN.md 52): the batch ends of one drawn frame, by class.

    m37_ends.py LOG [LOG...]        an `--endlog F` log per scene

`--endlog F` prints one line per batch submitted on drawn frame F:

    endlog> b1234 who=GXSetTevDirect verts=24 segs=1 calls=1 prim=98 \
            rec=xxxxxxxx lay=xxxxxxxx mtx=xxxxxxxx

`rec` is a hash of exactly the bytes the submit reads (M22's SubmitRec: the
transform, the raster state, the whole TEV chain with the bound textures'
contents, the channels, the alpha test, the z mode, the blend, the cull),
`lay` the vertex layout, `mtx` the batch's own position and normal matrices.

Two *consecutive* batches with the same `rec` and `lay` could have been one
batch -- the same draws in the same order, one boundary fewer -- if their
vertices are contiguous in the ring, which they are unless the layout moved.
If their `mtx` agrees too the merge needs nothing at all; if it does not, it
needs the matrix to be carried (the palette, or M22's pre-transform), which
is the harder half.  Each such pair is charged to the setter named on the
*second* line, which is the one that ended the first batch's run.
"""
import re, sys, collections

LINE = re.compile(r'endlog> b(\d+)\s+who=(\S+)\s+verts=(\d+)\s+segs=(\d+)\s+calls=(\d+)\s+'
                  r'prim=(\w+)\s+rec=(\w+)\s+notex=(\w+)\s+lay=(\w+)\s+mtx=(\w+)'
                  r'\s+tex=(\d+)x(\d+)/(\d+)/(\d+)(.*)')
# ` u0=64x32/f4/w00/ci0/g17` per stage in use
UNIT = re.compile(r'u(\d+)=(\d+)x(\d+)/f(\d+)/w(\d)(\d)/ci(\d)/g(\d+)')


def parse(path):
    """the log's endlog lines, split into the frames they belong to (the
    drawn frames of --endlog are hundreds of frames apart, so a gap of more
    than fifty log lines between two endlog lines is a frame boundary)"""
    out = []
    frames = []
    last = None
    for ln, raw in enumerate(open(path, errors='replace')):
        m = LINE.search(raw)
        if m:
            if last is not None and ln - last > 50:
                frames.append(out)
                out = []
            last = ln
            out.append(dict(n=int(m.group(1)), who=m.group(2), verts=int(m.group(3)),
                            segs=int(m.group(4)), calls=int(m.group(5)), prim=m.group(6),
                            rec=m.group(7), notex=m.group(8), lay=m.group(9),
                            mtx=m.group(10),
                            tex=(int(m.group(11)), int(m.group(12)), int(m.group(13)),
                                 int(m.group(14))),
                            units=[tuple(int(x) for x in u)
                                   for u in UNIT.findall(m.group(15) or '')]))
    if out:
        frames.append(out)
    return frames


def report(path, bs):
    calls = sum(b['calls'] for b in bs)
    print("%s: %d batches, %d GL draw calls, %d segments, %d vertices" %
          (path, len(bs), calls, sum(b['segs'] for b in bs), sum(b['verts'] for b in bs)))
    if not bs:
        return
    # a run is a maximal sequence of consecutive batches with the same rec+lay
    free = collections.Counter()      # rec+lay+mtx all equal: needs nothing
    needs_mtx = collections.Counter() # rec+lay equal, mtx moved
    free_calls = needs_calls = 0
    runs = 1
    runs_mtx = 1
    for a, b in zip(bs, bs[1:]):
        same = a['rec'] == b['rec'] and a['lay'] == b['lay']
        if not same:
            runs += 1
            runs_mtx += 1
            continue
        if a['mtx'] == b['mtx']:
            free[b['who']] += 1
            free_calls += b['calls']
        else:
            runs_mtx += 1
            needs_mtx[b['who']] += 1
            needs_calls += b['calls']
    print("  runs of equal state            %4d   (calls above the floor: %d)" %
          (runs, calls - runs))
    print("  runs of equal state and matrix %4d   (calls above that floor: %d)" %
          (runs_mtx, calls - runs_mtx))
    print("  batch ends the state did not need: %d   (%d of them need no matrix work,"
          " %d need the matrix carried)" %
          (sum(free.values()) + sum(needs_mtx.values()), sum(free.values()),
           sum(needs_mtx.values())))
    print("  %-28s %8s %8s %8s %8s" % ("ended by", "total", "free", "needs mtx", "calls"))
    tot = collections.Counter(b['who'] for b in bs)
    for who, n in tot.most_common():
        f, m = free[who], needs_mtx[who]
        if f or m:
            print("  %-28s %8d %8d %8d %8d" %
                  (who, n, f, m,
                   sum(b['calls'] for b in bs if b['who'] == who)))
    print("  %-28s %8d %8d %8d" % ("-- setters that end nothing", 0, 0, 0))
    for who, n in tot.most_common():
        if not free[who] and not needs_mtx[who]:
            print("     %-25s %8d" % (who, n))
    print("  free calls %d, calls needing the matrix %d" % (free_calls, needs_calls))
    atlas_pairs = 0
    atlas_foldable = 0
    atlas_tex = {}
    fold_tex = {}
    reasons = collections.Counter()

    def foldable(a, b):
        """could one atlas page serve both draws, exactly?

        Every texture reaches GL as GL_RGBA8 decoded on the CPU, so the
        format and the palette decide nothing.  The texels are the same
        bytes in a page as out of it, so the only questions are whether the
        page can reproduce the *sampling*: GL_CLAMP_TO_EDGE on both axes (a
        repeated or mirrored texture tiles the whole page, which no
        sub-rectangle can do), the same unit set, and a size that is a tile
        of a 1024x1024 page (with a one-texel border, so a bilinear tap at
        a tile's edge reads the tile's own edge texel and not its
        neighbour's).
        """
        ua, ub = a['units'], b['units']
        if len(ua) != len(ub) or not ua:
            reasons['different unit sets'] += 1
            return False
        if [u[0] for u in ua] != [u[0] for u in ub]:
            reasons['different unit sets'] += 1
            return False
        if all(x[7] == y[7] for x, y in zip(ua, ub)):
            reasons['same textures (not a texture boundary)'] += 1
            return False
        for x, y in zip(ua, ub):
            if x[7] == y[7]:
                continue            # this unit does not move: it can stay bound
            # the format and the palette do not decide: every texture in this
            # port is decoded on the CPU and uploaded as GL_RGBA8 (gx_tex.c),
            # so a CI texture is an ordinary image by the time GL sees it
            if x[4] or x[5] or y[4] or y[5]:
                reasons['wrap is not clamp on both axes'] += 1
                return False
            if max(x[1], x[2], y[1], y[2]) > 256:
                reasons['a texture larger than a 256 tile'] += 1
                return False
        return True

    for a, b in zip(bs, bs[1:]):
        if a['rec'] == b['rec'] and a['lay'] == b['lay']:
            continue
        if a['notex'] != b['notex'] or a['lay'] != b['lay']:
            continue
        atlas_pairs += 1
        for d in (a, b):
            for u in d['units']:
                atlas_tex[u[7]] = u[1:3]
        if a['mtx'] != b['mtx']:
            # the matrices end the batch anyway: folding the texture buys
            # nothing without the palette or the pre-transform as well
            reasons['the matrices end the batch anyway'] += 1
            continue
        if foldable(a, b):
            atlas_foldable += 1
            for d in (a, b):
                for u in d['units']:
                    fold_tex[u[7]] = u[1:3]
    small = {g: t for g, t in atlas_tex.items() if t[0] <= 256 and t[1] <= 256}
    area = sum((t[0] + 2) * (t[1] + 2) for t in fold_tex.values())
    print("  the atlas question: %d consecutive run boundaries differ in the texture alone"
          " (%.0f%% of the %d runs); %d of them a page could actually fold (%.0f%%)" %
          (atlas_pairs, 100.0 * atlas_pairs / max(runs, 1), runs, atlas_foldable,
           100.0 * atlas_foldable / max(runs, 1)))
    for r, n in reasons.most_common():
        print("     not foldable: %-45s %4d" % (r, n))
    print("     the foldable boundaries' textures: %d distinct, %d texels with a"
          " one-texel border each (%.2f pages of 1024x1024)" %
          (len(fold_tex), area, area / (1024.0 * 1024.0)))


def main():
    names = ['title (800)', 'character select (3000)', 'board (7000)']
    for p in sys.argv[1:]:
        for i, bs in enumerate(parse(p)):
            report('%s  %s' % (p, names[i] if i < len(names) else 'frame %d' % i), bs)
            print()


if __name__ == '__main__':
    main()
