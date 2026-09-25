#!/usr/bin/env python3
"""M44 (PLAN.md 59): the GX front end's regions from `--pmc 1` logs.

    python3 port/tools/m44_pmc.py LOG [LOG...]

For each log (a K: run of tools/m44_chain.sh on a PMC_WRAP=1 bundle), the
drawn frame's regions: calls, M cycles, M instructions, IPC, and per draw
(the TEV applies stand for the draws: one gx_tev_apply per batch) the
instructions and cycles; then the GX total against the frame."""
import re
import sys


def read(path):
    drawn = {}
    frames = 0
    on = False
    for line in open(path, errors="replace"):
        m = re.match(r"port> pmc: (drawn|consumed) frames (\d+)", line)
        if m:
            on = m.group(1) == "drawn"
            if on:
                frames = int(m.group(2))
            continue
        if not on:
            continue
        m = re.match(r"port> pmc:   (.+?)\s+([\d.]+)\s+([\d.]+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s*$", line)
        if m:
            name = m.group(1).strip()
            drawn[name] = dict(calls=float(m.group(2)), mcyc=float(m.group(3)),
                               instr=int(m.group(6)), l1=int(m.group(5)), l3=int(m.group(8)))
    return frames, drawn


def main():
    for path in sys.argv[1:]:
        frames, d = read(path)
        if not d:
            print("%s: no drawn-frame table" % path)
            continue
        draws = d.get("GX tev", {}).get("calls", 0) or 1
        gx = {k: v for k, v in d.items() if k == "port GX" or k.startswith("GX ")}
        tot_c = sum(v["mcyc"] for v in gx.values())
        tot_i = sum(v["instr"] for v in gx.values())
        frame = d.get("(total)", {})
        print("%s: %d drawn frames, %.0f draws a frame; GX %.2f M cycles / %.2f M instr "
              "(IPC %.2f) of %.2f / %.2f; %.0f instr, %.0f cycles a draw"
              % (path.split("/")[-1], frames, draws, tot_c, tot_i / 1e6,
                 tot_i / (tot_c * 1e6) if tot_c else 0, frame.get("mcyc", 0),
                 frame.get("instr", 0) / 1e6, tot_i / draws, tot_c * 1e6 / draws))
        for k, v in sorted(gx.items(), key=lambda kv: -kv[1]["mcyc"]):
            print("  %-22s %7.1f calls  %6.3f Mcyc  %6.3f Minstr  IPC %.2f  %6.0f instr/draw  L3 %6d"
                  % (k, v["calls"], v["mcyc"], v["instr"] / 1e6,
                     v["instr"] / (v["mcyc"] * 1e6) if v["mcyc"] else 0, v["instr"] / draws, v["l3"]))


if __name__ == "__main__":
    main()
