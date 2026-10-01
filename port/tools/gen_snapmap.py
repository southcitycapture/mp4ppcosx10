#!/usr/bin/env python3
"""Work out which of the executable's writable globals belong to the *game*.

A snapshot (PLAN.md 24.2) has to carry every byte of state the game can
observe.  Most of it is in MEM1 and ARAM, which the port owns and can copy
wholesale.  The rest is in the main binary's `__DATA`: on the console the DOL's
`.data`/`.bss`/`.sdata` lived in MEM1 at fixed addresses, and here they are
ordinary globals linked into the port's own executable -- `GWGameStat`,
`Hu3DData`, the overlay table, MusyX's voice and sequencer state, all of it.

Which is a problem, because the *port's* globals are in the same segment and
must emphatically NOT be restored: they hold this process's SDL window, its GL
texture names, its open `FILE*`s, its `argv` strings.  Restoring a previous
process's copies of those would hand the new process a fistful of dangling
pointers.

The linker already knows the answer.  `ld -map` prints, for every symbol, its
address, its size and the object file it came from; the object file says
`build-ppc-darwin/game/...` or `build-ppc-darwin/musyx/...` for the game and
`build-ppc-darwin/port/...` for the port.  So this reads the map after the link
and writes the game-owned ranges next to the binary as `<exe>.snapmap`, which
`port/src/debug/snapshot.c` loads at boot.

A sidecar rather than a generated `.c` on purpose: a table compiled *into* the
binary would move the very addresses it describes, and the second link would
need a third.

Usage: gen_snapmap.py --map build-ppc-darwin/marioparty4.map \
                      --exe build-ppc-darwin/marioparty4 \
                      --out build-ppc-darwin/marioparty4.snapmap
"""
import argparse
import os
import re
import sys

# The writable, stateful sections.  __const is writable in Mach-O's __DATA on
# this target but the game never writes it (it is `const` data that needed a
# relocation), and __dyld / __*_symbol_ptr belong to the loader.
DATA_SECTIONS = ("__data", "__bss", "__common")


def parse_map(path):
    objs, sections, symbols = {}, [], []
    mode = None
    with open(path, "r", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("# Object files:"):
                mode = "obj"
                continue
            if line.startswith("# Sections:"):
                mode = "sec"
                continue
            if line.startswith("# Symbols:"):
                mode = "sym"
                continue
            if not line or line.startswith("#"):
                continue
            if mode == "obj":
                m = re.match(r"\[\s*(\d+)\]\s+(.*)", line)
                if m:
                    objs[int(m.group(1))] = m.group(2)
            elif mode == "sec":
                parts = line.split()
                if len(parts) >= 4:
                    sections.append((int(parts[0], 16), int(parts[1], 16),
                                     parts[2], parts[3]))
            elif mode == "sym":
                m = re.match(r"(0x[0-9A-Fa-f]+)\s+(0x[0-9A-Fa-f]+)\s+\[\s*(\d+)\]\s+(.*)",
                             line)
                if m:
                    symbols.append((int(m.group(1), 16), int(m.group(2), 16),
                                    int(m.group(3)), m.group(4)))
    return objs, sections, symbols


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", required=True)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    objs, sections, symbols = parse_map(args.map)
    data = [s for s in sections if s[2] == "__DATA" and s[3] in DATA_SECTIONS]
    if not data:
        sys.stderr.write("gen_snapmap: the map has no __DATA sections\n")
        return 1

    def in_data(addr):
        for lo, size, _seg, _sec in data:
            if lo <= addr < lo + size:
                return True
        return False

    def is_game(idx):
        p = objs.get(idx, "")
        return "/game/" in p or "/musyx/" in p or p.endswith("/musyx.o")

    ranges = []
    for addr, size, idx, name in symbols:
        if size == 0 or not in_data(addr) or not is_game(idx):
            continue
        ranges.append((addr, size))
    ranges.sort()

    merged = []
    for addr, size in ranges:
        if merged and addr <= merged[-1][0] + merged[-1][1] + 32:
            lo, ln = merged[-1]
            merged[-1] = (lo, max(ln, addr + size - lo))
        else:
            merged.append((addr, size))

    total = sum(n for _a, n in merged)
    exe_size = os.path.getsize(args.exe)
    with open(args.out, "w") as f:
        f.write("# mp4 snapmap 1 -- the game-owned writable globals of the "
                "main binary (port/tools/gen_snapmap.py)\n")
        f.write("exe-size %d\n" % exe_size)
        f.write("ranges %d\n" % len(merged))
        f.write("bytes %d\n" % total)
        for addr, size in merged:
            f.write("r %08x %x\n" % (addr, size))
    if not args.quiet:
        print("snapmap: %d game data ranges, %d KB of %d symbols -> %s"
              % (len(merged), total // 1024, len(symbols), args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
