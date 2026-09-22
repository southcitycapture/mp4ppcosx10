#!/usr/bin/env python3
"""The soak's read (PLAN.md 45.1): from a --soak --status log (gz or plain), the
speed and presented fps per board turn and per minigame, the render thread's
rt/dec medians, and every line that outranks a number (resync, fault, STUCK,
stall over a second, CARD flush, mismatch)."""
import gzip, re, sys, statistics as st
path = sys.argv[1]
f = gzip.open(path, 'rt', errors='replace') if path.endswith('.gz') else open(path, errors='replace')
st_re = re.compile(r'status f(\d+)\s+(\S+)\s+board (\d+) turn (\d+)/(\d+)\s+mg (\d+) \((\S+)\).*?speed (\d+)%\s+([\d.]+) fps presented.*?rss (\d+) MB.*?rt ([\d.]+) ms dec ([\d.]+)')
turns = {}; games = {}; events = []; n = 0; first = last = None
ur_re = re.compile(r'machine \S+\s+ur (\d+)'); ur_prev = None; ur_by = {}  # M39: underruns per scene
rss_t = []  # M39: (frame, rss) for the rss-over-time line
enter = re.compile(r'soak: enter minigame (\S+)\s+at frame (\d+)')
for line in f:
    m = st_re.search(line)
    if m:
        fr, ovl, board, turn, tmax, mg, mgn, speed, fps, rss, rt, dec = m.groups()
        n += 1; last = int(fr)
        if first is None: first = int(fr)
        key = (ovl if not ovl.startswith('m4') else ovl, int(turn)) if ovl.startswith('w0') else (ovl, 0)
        d = games if ovl.startswith('m4') else turns
        k = ovl if ovl.startswith('m4') else 'turn %02d (%s)' % (int(turn), ovl)
        d.setdefault(k, []).append((int(speed), float(fps), float(rt), float(dec), int(rss)))
        rss_t.append((int(fr), int(rss)))
        mu = ur_re.search(line)
        if mu:
            u = int(mu.group(1))
            if ur_prev is not None and u > ur_prev:
                ur_by[k] = ur_by.get(k, 0) + u - ur_prev
            ur_prev = u
        continue
    if re.search(r'resync|\*\*\* port|signal \d+|STUCK|mismatch|CARD: image flush|late \(|guard', line):
        events.append(line.rstrip()[:200])
    m2 = re.search(r'stall: frame (\d+) took (\d+) ms', line)
    if m2 and int(m2.group(2)) >= 1000:
        events.append(line.rstrip()[:200])
def row(k, v):
    return "%-24s n %4d  speed %5.1f%%  fps %5.1f (min %4.1f)  rt %5.1f  dec %4.1f  rss %d" % (
        k, len(v), st.mean(x[0] for x in v), st.mean(x[1] for x in v), min(x[1] for x in v),
        st.median(x[2] for x in v), st.median(x[3] for x in v), max(x[4] for x in v))
allv = [x for v in list(turns.values()) + list(games.values()) for x in v]
print("status lines %d, frames %s..%s, mean speed %.1f%%, mean fps %.1f" % (n, first, last, st.mean(x[0] for x in allv), st.mean(x[1] for x in allv)))
print("\nboard turns / screens:")
for k in sorted(turns): print("  " + row(k, turns[k]))
print("\nminigames:")
for k in sorted(games): print("  " + row(k, games[k]))
print("\nevents (%d):" % len(events))
for e in events: print("  " + e)
if ur_by:
    hours = (last - first) / 59.94 / 3600 if last and first is not None and last > first else 0
    tot = sum(ur_by.values())
    print("\nunderruns (the status line's ur field, M39): %d in all%s" % (tot, (", %.1f an hour" % (tot / hours)) if hours else ""))
    for k in sorted(ur_by, key=lambda k: -ur_by[k]): print("  %-24s %d" % (k, ur_by[k]))
if rss_t:
    step = max(1, len(rss_t) // 12)
    print("\nrss over the run (frame:MB): " + "  ".join("%d:%d" % x for x in rss_t[::step] + [rss_t[-1]]))

