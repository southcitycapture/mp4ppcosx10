#!/usr/bin/env python3
"""M36 (PLAN.md 51.3): what a game actually reads -- the per-file counts of a
--dvdlog run, the resident list they imply for a budget, and the files each
screen reads (by the --ovllog transitions).

    m36_dvdlog.py LOG [--budget MB] [--fst FST.txt] [--list OUT.h]

`port> dvd: fN path +off len ms disk|resident` lines are counted per file;
the list is the files sorted by reads (bytes as the tie-break) taken in
order while they fit the budget (a file larger than what is left is skipped,
not the ones after it).  --fst is the FST dump (path\\toffset\\tlength) for
the sizes of files the run never read; --list writes resident_list.h.
"""
import re, sys, collections

args = sys.argv[1:]
log = args[0]
budget = 256
fst = None
out = None
i = 1
while i < len(args):
    if args[i] == '--budget':
        budget = int(args[i + 1]); i += 2
    elif args[i] == '--fst':
        fst = args[i + 1]; i += 2
    elif args[i] == '--list':
        out = args[i + 1]; i += 2
    else:
        i += 1

sizes = {}
if fst:
    for line in open(fst):
        p, o, l = line.rstrip('\n').split('\t')
        sizes[p.lower()] = int(l)

reads = collections.Counter()
bytes_ = collections.Counter()
slow = collections.Counter()
ms = collections.Counter()
whole = collections.Counter()   # reads at offset 0 of the whole file
name_of = {}
scene = None
scene_files = collections.OrderedDict()
frames_of_scene = {}
first_frame = {}
rx = re.compile(r'port> dvd: f(\d+) (\S+) \+(\d+) (\d+) (?:([\d.]+) ms )?(disk|resident)')
ovl = re.compile(r'port> frame (\d+): overlay (-?\d+) \(next (-?\d+)\)')
for line in open(log, errors='replace'):
    m = ovl.search(line)
    if m:
        scene = int(m.group(3))
        scene_files.setdefault(scene, collections.Counter())
        continue
    m = rx.search(line)
    if not m:
        continue
    fr, path, off, ln, t, src = m.groups()
    k = path.lower()
    name_of[k] = path
    reads[k] += 1
    bytes_[k] += int(ln)
    first_frame.setdefault(k, int(fr))
    if t:
        ms[k] += float(t)
        if float(t) >= 100:
            slow[k] += 1
    if sizes.get(k) and int(off) == 0 and int(ln) >= sizes[k] - 31:
        whole[k] += 1
    if scene is not None:
        scene_files[scene][k] += 1

print("%d files read, %d reads, %.1f MB, %.0f ms on the disk, %d reads over 100 ms"
      % (len(reads), sum(reads.values()), sum(bytes_.values()) / 1e6, sum(ms.values()), sum(slow.values())))
order = sorted(reads, key=lambda k: (-reads[k], -bytes_[k]))
print("\n%-28s %6s %10s %8s %7s %6s %s" % ("file", "reads", "bytes read", "size", "ms", "whole", "first"))
for k in order:
    print("%-28s %6d %10d %8s %7.0f %6d %s" % (name_of[k], reads[k], bytes_[k],
          (sizes.get(k, 0) // 1024) if sizes else '?', ms[k], whole[k], first_frame[k]))

# the list for the budget
chosen = []
left = budget * 1024 * 1024
for k in order:
    sz = sizes.get(k)
    if sz is None:
        continue
    if sz <= left:
        chosen.append(k)
        left -= sz
print("\nlist for %d MB: %d files, %.1f MB, %d of the run's %d reads (%.1f%%) would be served"
      % (budget, len(chosen), (budget * 1024 * 1024 - left) / 1048576.0,
         sum(reads[k] for k in chosen), sum(reads.values()),
         100.0 * sum(reads[k] for k in chosen) / max(1, sum(reads.values()))))
for b in (64, 128, 256):
    left2 = b * 1024 * 1024
    ch = []
    for k in order:
        sz = sizes.get(k)
        if sz is not None and sz <= left2:
            ch.append(k); left2 -= sz
    print("  at %3d MB: %3d files, %5.1f MB, %5.1f%% of reads, %5.1f%% of bytes" % (
        b, len(ch), (b * 1024 * 1024 - left2) / 1048576.0,
        100.0 * sum(reads[k] for k in ch) / max(1, sum(reads.values())),
        100.0 * sum(bytes_[k] for k in ch) / max(1, sum(bytes_.values()))))

if out:
    with open(out, 'w') as f:
        f.write("/* M36 (PLAN.md 51.3): the resident set's list, most-read first, as\n"
                " * port/tools/m36_dvdlog.py counted the game's reads per file over a\n"
                " * --dvdlog run of the deterministic 20-turn soak (%s).  dvd_cache.c\n"
                " * fills it in this order until the budget is spent; a file larger than\n"
                " * what is left is skipped.  Generated -- edit the run, not the list. */\n"
                % log.split('/')[-1])
        for k in order:
            if k in sizes:
                f.write('"%s", /* %d reads, %d KB */\n' % (name_of[k], reads[k], sizes[k] // 1024))
    print("wrote", out)

print("\nfiles by the screen that was next when they were read (reads):")
names = {}
for s, c in scene_files.items():
    if not c:
        continue
    print("  next %3d: %s" % (s, ', '.join('%s x%d' % (name_of[k].split('/')[-1], n) for k, n in c.most_common(12))))
