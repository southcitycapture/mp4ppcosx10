#!/usr/bin/env python3
"""Diff the port's m444 ball trace against the console capture (PLAN.md 28.2).

The port's trace lines come from port/patches.txt's `m444trace` OSReport, which
prints the state at the *top* of `fn_1_8DD0`'s loop -- i.e. before the frame's
own integration -- as thousandths:

    port> m444trace <round> <n> vel <vx> <vy> pos <px> <py>

The console table (port/ref/m444-ball-console.csv) is MemoryWatcher's change
rows, read after the frame.  So port index n lines up with console frame
(launch_frame - 1 + n), and the alignment is checked rather than assumed: the
script anchors on the first row whose velocity is the launch velocity.

    m444diff.py PORTLOG CSV [--round N] [--launch F] [--rows N]
"""
import csv, re, sys, argparse

TRACE = re.compile(r"m444trace (\d+) (\d+) vel (-?\d+) (-?\d+) pos (-?\d+) (-?\d+)")
LAUNCH = re.compile(r"m444: launch round (\d+) .*charge (-?\d+)/1000 +temp_r24 (-?\d+) +frand (-?\d+) +vy (-?\d+)/1000")


def load_port(path, want_round):
    rows, launches = [], []
    for line in open(path, errors="replace"):
        m = LAUNCH.search(line)
        if m:
            launches.append(tuple(int(x) for x in m.groups()))
        m = TRACE.search(line)
        if m:
            r, n, vx, vy, px, py = (int(x) for x in m.groups())
            if r == want_round:
                rows.append((n, vx / 1000.0, vy / 1000.0, px / 1000.0, py / 1000.0))
    return rows, launches


def load_console(path):
    out = []
    with open(path) as f:
        for row in csv.DictReader(r for r in f if not r.startswith("#")):
            out.append((int(row["frame"]), float(row["velx"]), float(row["vely"]),
                        float(row["posx"]), float(row["posy"])))
    return out


def expand(rows, first, last):
    """MemoryWatcher writes change rows only; hold the last value across gaps."""
    out, i = {}, 0
    cur = rows[0][1:]
    for f in range(first, last + 1):
        while i < len(rows) and rows[i][0] <= f:
            cur = rows[i][1:]
            i += 1
        out[f] = cur
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("portlog"); ap.add_argument("csv")
    ap.add_argument("--round", type=int, default=0)
    ap.add_argument("--launch", type=int, default=12230)
    ap.add_argument("--rows", type=int, default=40)
    a = ap.parse_args()

    port, launches = load_port(a.portlog, a.round)
    con = load_console(a.csv)
    if not port:
        sys.exit("no m444trace rows for round %d in %s" % (a.round, a.portlog))
    for L in launches:
        print("port launch: round %d charge %.3f temp_r24 %d frand %d vy %.3f"
              % (L[0], L[1] / 1000.0, L[2], L[3], L[4] / 1000.0))
    cmap = expand(con, con[0][0], con[-1][0])

    print("\n  n |  frame |        port vel x,y        |      console vel x,y       "
          "|        port pos x,y        |      console pos x,y")
    worst = 0.0
    for n, vx, vy, px, py in port[: a.rows]:
        f = a.launch - 1 + n
        if f not in cmap:
            break
        cvx, cvy, cpx, cpy = cmap[f]
        d = max(abs(vx - cvx), abs(vy - cvy), abs(px - cpx), abs(py - cpy))
        worst = max(worst, d)
        print("%3d | %6d | %11.3f %11.3f | %11.3f %11.3f | %11.3f %11.3f | %11.3f %11.3f  %s"
              % (n, f, vx, vy, cvx, cvy, px, py, cpx, cpy, "" if d < 0.02 else "<-- %.3f" % d))
    print("\nworst absolute difference over %d rows: %.4f" % (min(a.rows, len(port)), worst))


main()
