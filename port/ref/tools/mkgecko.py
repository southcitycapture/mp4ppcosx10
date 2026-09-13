#!/usr/bin/env python3
"""
mkgecko.py -- compile a frame-numbered input script into Dolphin Gecko codes
that inject controller state straight into Mario Party 4's pad globals.

Why this exists: Dolphin 2506-433's `-m <movie.dtm>` command-line playback does
not take effect in this build (see port/docs/reference-dolphin.md §5), and the
GUI recorder is not reachable from a script. Gecko codes are, and they turn out
to be a *better* fit for a port reference anyway: the condition is the game's own
`GlobalCounter`, so the input schedule is expressed in the same frame numbers the
port will count.

How it works. `HuPadRead()` (src/game/pad.c:130) copies the private `_Pad*`
arrays into the public `HuPad*` globals once per frame and then clears
`_PadBtnDown`. Dolphin's Gecko code handler runs at the VI hook, which lands
between `PadReadVSync()` filling `_Pad*` and the next frame's `HuPadRead()`
reading them — so writing `_PadBtn` / `_PadBtnDown` / `_PadDStk` from a Gecko
code is indistinguishable, to the game, from a real button press. Verified: a
code forcing START skips both logos, skips the opening THP movie, and accepts
the title screen.

Script syntax -- one directive per line, '#' starts a comment:

    at <frame> <frames> <btn>[,<btn>...]   # hold buttons for <frames> frames
                                           #   starting at GlobalCounter == <frame>
    at <frame> <frames> dstk:<dir>         # hold a stick direction (menu movement)
    every <period> <frames> <from> <btn> [<phase>]
                                           # hold buttons for <frames> frames out of
                                           #   every <period>, from <from> onwards,
                                           #   offset <phase> frames into the period
    mark <frame> <label>                   # emit a comment; also written to .marks

`every` exists because the code list has to fit in Dolphin's Gecko region.  The
handler is injected at 0x80001800 and the codes live behind it in the same
few-kilobyte window; a list that does not fit is **silently not installed**, and
the run then looks exactly like a run with no codes at all -- the attract loop.
An `at` costs five lines, so an A press every 90 frames out of 13,000 is 715
lines and over the edge, while the same metronome as one `every` is five lines
in total.  It is compiled as a bit test on the low half of `GlobalCounter`
(Gecko `28`, "if (u16 & ~mask) == value"), so <period> must be a power of two
and <frames> a power of two no greater than it.

Buttons: A B X Y Z L R START.  Directions: UP DOWN LEFT RIGHT.

Usage:  mkgecko.py script.txt out.ini [--name ForceInput]

The output is a Dolphin per-game ini. Drop it in <userdir>/GameSettings/GMPE01.ini
and run with `-C Dolphin.Core.EnableCheats=True`.
"""
import argparse, sys

# Rev 1 USA addresses, from config/GMPE01_01/symbols.txt
GLOBAL_COUNTER = 0x801D3A54  # u32, src/game/main.c:119
PAD_BTN        = 0x801D3A9C  # u16[4] _PadBtn
PAD_BTN_DOWN   = 0x801D3A94  # u16[4] _PadBtnDown
PAD_DSTK       = 0x801D3A70  # s8[4]  _PadDStk
PAD_DSTK_REP   = 0x801D3A6C  # s8[4]  _PadDStkRep

# Dolphin SDK PAD_BUTTON_* bits
BTN = {
    'LEFT': 0x0001, 'RIGHT': 0x0002, 'DOWN': 0x0004, 'UP': 0x0008,
    'Z': 0x0010, 'R': 0x0020, 'L': 0x0040,
    'A': 0x0100, 'B': 0x0200, 'X': 0x0400, 'Y': 0x0800, 'START': 0x1000,
}
# HuPadDStk direction bits reuse the D-pad bit positions
DSTK = {'LEFT': 0x01, 'RIGHT': 0x02, 'DOWN': 0x04, 'UP': 0x08}


def off(addr):
    """Gecko codes address relative to the 0x80000000 base register."""
    return addr & 0x01FFFFFF


def block(first, last, writes):
    """Emit `if first <= GlobalCounter <= last` around a list of (addr, size, value)."""
    out = [f'24{off(GLOBAL_COUNTER):06X} {first - 1:08X}',   # if u32 > first-1
           f'26{off(GLOBAL_COUNTER):06X} {last + 1:08X}']    # if u32 < last+1
    for addr, size, value in writes:
        if size == 1:
            out.append(f'00{off(addr):06X} 0000{value:02X}')
        elif size == 2:
            out.append(f'02{off(addr):06X} 0000{value:04X}')
        else:
            raise ValueError(size)
    out.append('E0000000 80008000')                          # end all conditionals
    return out


def every_block(period, hold, first, writes, phase=0):
    """A metronome, as one masked bit test rather than one block per press.

    `GlobalCounter % period < hold` is a test on the counter's low bits when
    both are powers of two: the bits from `hold` up to `period` must all be
    zero.  Gecko's 16-bit conditional takes a mask of bits to *ignore*, so the
    mask is the complement of that band, tested against zero, on the low half
    of the counter (big-endian, so +2)."""
    if period & (period - 1) or hold & (hold - 1) or hold > period:
        raise ValueError('every: period and frames must be powers of two, frames <= period')
    if phase % hold or phase >= period:
        raise ValueError('every: phase must be a multiple of frames and below period')
    band = (period - 1) & ~(hold - 1)          # the bits that select the slot
    ignore = 0xFFFF & ~band                    # Gecko's mask is what to ignore
    out = [f'24{off(GLOBAL_COUNTER):06X} {first - 1:08X}',
           f'28{off(GLOBAL_COUNTER) + 2:06X} {ignore:04X}{phase:04X}']
    for addr, size, value in writes:
        if size == 1:
            out.append(f'00{off(addr):06X} 0000{value:02X}')
        elif size == 2:
            out.append(f'02{off(addr):06X} 0000{value:04X}')
        else:
            raise ValueError(size)
    out.append('E0000000 80008000')
    return out


def pad_writes(what):
    writes = []
    if what.lower().startswith('dstk:'):
        for d in what.split(':', 1)[1].upper().split(','):
            v = DSTK[d]
            writes.append((PAD_DSTK, 1, v))
            writes.append((PAD_DSTK_REP, 1, v))
    else:
        mask = 0
        for b in what.upper().split(','):
            mask |= BTN[b]
        writes.append((PAD_BTN, 2, mask))
        writes.append((PAD_BTN_DOWN, 2, mask))
    return writes


def compile_script(path):
    lines, marks = [], []
    with open(path) as f:
        for lineno, raw in enumerate(f, 1):
            s = raw.split('#', 1)[0].strip()
            if not s:
                continue
            p = s.split()
            try:
                if p[0].lower() == 'mark':
                    marks.append((int(p[1]), ' '.join(p[2:])))
                    lines.append(f'* frame {int(p[1])}: {" ".join(p[2:])}')
                    continue
                if p[0].lower() == 'every':
                    period, hold, first, what = int(p[1]), int(p[2]), int(p[3]), p[4]
                    phase = int(p[5]) if len(p) > 5 else 0
                    lines += every_block(period, hold, first, pad_writes(what), phase)
                    continue
                if p[0].lower() != 'at':
                    raise ValueError(f'unknown directive {p[0]!r}')
                first, count, what = int(p[1]), int(p[2]), p[3]
                last = first + count - 1
                lines += block(first, last, pad_writes(what))
            except Exception as e:
                sys.exit(f'{path}:{lineno}: {e}')
    return lines, marks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('script')
    ap.add_argument('out')
    ap.add_argument('--name', default='RefInput')
    a = ap.parse_args()

    lines, marks = compile_script(a.script)
    with open(a.out, 'w') as f:
        f.write('# Generated by port/ref/tools/mkgecko.py -- do not edit by hand.\n')
        f.write(f'# Source: {a.script}\n')
        f.write('[Gecko]\n')
        f.write(f'${a.name}\n')
        f.write('\n'.join(lines) + '\n')
        f.write('[Gecko_Enabled]\n')
        f.write(f'${a.name}\n')
    with open(a.out + '.marks', 'w') as f:
        for fr, label in marks:
            f.write(f'{fr}\t{label}\n')
    print(f'{a.out}: {len(lines)} code lines, {len(marks)} marks')


if __name__ == '__main__':
    main()
