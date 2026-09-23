#!/usr/bin/env python3
"""M39c (PLAN.md 54b.2): one board's realtime run, read.

    m39c_read.py LOG [LOG...]      (plain or .gz)

Per log: the board, the turns it reached, faults, STUCK lines (and on which
screen), resyncs and the worst late, speed and presented fps over the
board's own status lines (mean speed; fps median / p10 / min), rt / dec
medians, audio underruns on the board, the minigames entered, and the
board's events as the log shows them -- the event files it read
(`--dvdlog`: bkoopa = Bowser's space, byokodori = Boo, bkujiya = the
lottery, bbattle = a battle space, blast5 = the last five turns, bguest,
bkoopasuit) and the game's own OSReport lines that name one."""
import sys, re, gzip, statistics as st

EVENT_FILES = {
    'bkoopa': "Bowser's space (bkoopa)", 'bkoopasuit': 'Koopa Kid / suit (bkoopasuit)',
    'byokodori': 'Boo (byokodori)', 'bkujiya': 'the lottery (bkujiya)',
    'bbattle': 'a battle space (bbattle)', 'blast5': 'the last five turns (blast5)',
    'bguest': 'the guest (bguest)',
}


def opener(p):
    return gzip.open(p, 'rt', errors='replace') if p.endswith('.gz') else open(p, errors='replace')


def pct(xs, q):
    if not xs:
        return float('nan')
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(q * (len(xs) - 1) + 0.5))]


def read(path):
    board = None; turn_max = 0; max_turn = 0
    fps = []; speed = []; rt = []; dec = []; ur0 = ur1 = None
    faults = []; stuck = []; resync = 0; worst_late = None
    mgs = []; roulette = []; events = {}; osr = {}; board_frames = 0
    first_f = last_f = None
    for line in opener(path):
        if line.startswith('port> status'):
            m = re.match(r'port> status f(\d+)\s+(\S+)\s+board (\d+) turn (\d+)/(\d+).*?speed (\d+)%\s+([\d.]+) fps presented', line)
            if not m:
                continue
            f, scr, b, t, mt = int(m.group(1)), m.group(2), int(m.group(3)), int(m.group(4)), int(m.group(5))
            first_f = first_f or f; last_f = f
            if re.match(r'w\d\ddll', scr):
                board = scr
                turn_max = max(turn_max, t); max_turn = mt
                speed.append(int(m.group(6))); fps.append(float(m.group(7))); board_frames += 60
                mm = re.search(r'rt ([\d.]+) ms dec ([\d.]+)', line)
                if mm:
                    rt.append(float(mm.group(1))); dec.append(float(mm.group(2)))
                mm = re.search(r'ur (\d+)', line)
                if mm:
                    u = int(mm.group(1)); ur0 = u if ur0 is None else ur0; ur1 = u
            continue
        if line.startswith('*** port:') or 'port: fatal' in line:
            faults.append(line.strip())
        elif 'STUCK:' in line:
            m = re.search(r'live screen (\S+)', line)
            stuck.append(m.group(1) if m else '?')
        elif 'realtime: resync' in line:
            resync += 1
        elif 'worst' in line and 'behind the schedule' in line:
            worst_late = line.strip()
        elif 'enter minigame' in line:
            m = re.search(r'enter minigame (\S+)', line)
            mgs.append(m.group(1))
        elif line.startswith('port> roulette: frame'):
            m = re.search(r'dealt mg (\d+) \((\S+), type', line)
            if m:
                roulette.append(m.group(2))
        elif line.startswith('port> dvd: '):
            m = re.search(r'data/(b[a-z0-9]+)\.bin \+0 ', line)
            if m and m.group(1) in EVENT_FILES:
                events[m.group(1)] = events.get(m.group(1), 0) + 1
        elif not line.startswith('port>'):
            s = line.strip()
            if re.search(r'(?i)\bboo\b|koopa|bank|bowser|shy ?guy|goomba|lottery|monkey|dolphin|condor|hotel|statue|event', s) \
                    and len(s) < 90 and 'SE Entry' not in s and '.rel' not in s and 'dll' not in s.lower():
                osr[s] = osr.get(s, 0) + 1
    return dict(path=path, board=board, turn=turn_max, max_turn=max_turn, faults=faults, stuck=stuck,
                resync=resync, worst_late=worst_late, n=len(fps),
                speed=st.mean(speed) if speed else float('nan'),
                fps_med=st.median(fps) if fps else float('nan'), fps_p10=pct(fps, 0.10),
                fps_min=min(fps) if fps else float('nan'),
                rt=st.median(rt) if rt else float('nan'), dec=st.median(dec) if dec else float('nan'),
                ur=(ur1 - ur0) if ur0 is not None else None, mgs=mgs, events=events, osr=osr,
                frames=(first_f, last_f))


if __name__ == '__main__':
    for p in sys.argv[1:]:
        r = read(p)
        print(f"== {p}: {r['board']}  turn {r['turn']}/{r['max_turn']}  frames {r['frames'][0]}..{r['frames'][1]}")
        print(f"   faults {len(r['faults'])}  STUCK {len(r['stuck'])} {sorted(set(r['stuck']))}  resyncs {r['resync']}")
        if r['worst_late']:
            print(f"   {r['worst_late']}")
        print(f"   board: {r['n']} s  speed {r['speed']:.1f}%  fps median {r['fps_med']:.1f} p10 {r['fps_p10']:.1f} "
              f"min {r['fps_min']:.1f}  rt {r['rt']:.1f} ms  dec {r['dec']:.1f} ms  underruns on the board {r['ur']}")
        print(f"   minigames entered: {' '.join(r['mgs'])}")
        print(f"   event files: {', '.join(f'{EVENT_FILES[k]} x{v}' for k, v in sorted(r['events'].items())) or 'none'}")
        for s, c in sorted(r['osr'].items(), key=lambda x: -x[1])[:15]:
            print(f"   osreport x{c}: {s}")
        for f in r['faults']:
            print(f"   FAULT: {f}")
