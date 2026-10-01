#!/usr/bin/env python3
"""M43 (PLAN.md 58.5): w01, the board, two ways -- the verdict rule is the
user's, so both numbers are reported and nothing is changed:

  pooled      what fps_board.py judges: every w01 status line of the chain at
              real time (speed <= 110%), the teleports' hand-over lines included;
  board-only  the same lines less those whose one-second window overlaps the
              first second after a fast-forward hands back to real time
              (a line at frame f covers (f-60, f]; the hand-back is --ffto's
              "drawing back on at frame H"; lines with f < H + 120 are out).

    m43_w01.py DIR_OR_LOGS...     (the scoreboard's logs, plain or .gz)
"""
import sys, os, re, glob, gzip, statistics as st
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fps_board

BACK = re.compile(r'ffto: reached frame \d+ .*drawing back on at frame (\d+)')

def main():
    paths = []
    for a in sys.argv[1:]:
        paths += sorted(glob.glob(os.path.join(a, '*.log*'))) if os.path.isdir(a) else [a]
    pooled, board, dropped = [], [], []
    per_run = {}
    for p in paths:
        stem = fps_board.stem_of(p)
        if fps_board.REP.match(stem) and fps_board.family_screen(fps_board.REP.match(stem).group(1)) != 'w01dll':
            continue  # repeats count their own screen only (as fps_board.py reads them)
        back = None
        for line in fps_board.opn(p):
            m = BACK.search(line)
            if m:
                back = int(m.group(1))
                continue
            m = fps_board.ST.search(line)
            if not m:
                continue
            f, ovl = int(m.group(1)), m.group(2)
            speed, fps = int(m.group(8)), float(m.group(9))
            if ovl != 'w01dll' or speed > 110:
                continue
            pooled.append(fps)
            if back is not None and f < back + 120:
                dropped.append((stem, f, speed, fps))
                continue
            board.append(fps)
            per_run.setdefault(stem, []).append(fps)
    print('w01 pooled:     %d lines, median %.1f, p10 %.1f' % (len(pooled), st.median(pooled), fps_board.pct(pooled, 0.1)))
    print('w01 board-only: %d lines, median %.1f, p10 %.1f  (%d hand-over lines left out, their median %.1f)' % (
        len(board), st.median(board), fps_board.pct(board, 0.1), len(dropped),
        st.median([d[3] for d in dropped]) if dropped else 0.0))
    print('runs with w01 lines kept (lines, median):', ', '.join('%s %d/%.1f' % (k, len(v), st.median(v))
                                                        for k, v in sorted(per_run.items(), key=lambda x: -len(x[1]))[:12]))

if __name__ == '__main__':
    main()
