#!/usr/bin/env python3
"""M41 (PLAN.md 56): split a `sample` thread's SELF time by where the code lives.

    python3 port/tools/m41_split.py SAMPLE.txt [--thread 0] [--map build-ppc-darwin/marioparty4.map] [--top 12]

  engine   the game's own code (src/game, src/REL module bundles, msm): Hu3DExec's
           walks, FaceDraw, the motion curves, the bone walk's own lines
  gx       the port's GX front end (port/src/gx: the state setters, gx_tev_apply,
           the texture binds, the decode, the vertex cache's keying, the stream)
  sdk      the SDK's math the port provides (game/src/dolphin/mtx, psmtx_c.c) and
           the libm it calls (sin/cos/exp/sqrt)
  audio    the mixer's control half on the game thread (port_audio_tick's subtree)
  other    the rest of the port (os, platform, dvd, pad), libSystem, SDL, the driver
The audio split is by subtree (anything under port_audio_tick); the rest by the
object file the linker map puts the symbol in.  Symbols not in the map are REL
module code (fn_*, the module's statics) unless they are a known system name."""
import re, sys, argparse, os
from collections import defaultdict
sys.path.insert(0, os.path.dirname(__file__))
from sample_tree import parse

LIBM = {"sin", "cos", "sinf", "cosf", "exp", "expf", "cexpf", "__sqrt", "sqrtf", "sqrt",
        "atan2", "atan2f", "atanf", "atan", "acosf", "acos", "asinf", "powf", "pow", "fmodf",
        "floorf", "ceilf", "fabs", "__kernel_sin", "__kernel_cos", "tanf", "tan", "log", "logf"}


def load_map(path):
    objs, sym = {}, {}
    sect = None
    for line in open(path, errors="replace"):
        if line.startswith("# Object files"):
            sect = "o"; continue
        if line.startswith("# Sections"):
            sect = None; continue
        if line.startswith("# Symbols"):
            sect = "s"; continue
        if sect == "o":
            m = re.match(r"\[\s*(\d+)\]\s+(.*)", line)
            if m:
                objs[int(m.group(1))] = m.group(2).strip()
        elif sect == "s":
            m = re.match(r"0x[0-9A-Fa-f]+\s+0x[0-9A-Fa-f]+\s+\[\s*(\d+)\]\s+(\S+)", line)
            if m:
                s = m.group(2)
                if s.startswith("_"):
                    s = s[1:]
                sym[s] = objs.get(int(m.group(1)), "")
    return sym


HELPER = re.compile(r"^(restGPRx|saveGPRx|restFP|saveFP|__memcpy|memcpy|memmove|__memmove|memcmp|"
                    r"__bzero|bzero|memset|__floatundidf|__floatdidf|__fixunsdfdi|__udivdi3|__divdi3|"
                    r"__umoddi3|dyld_stub_.*|.*\$stub|\?\?\?)$")


def klass(name, symmap):
    base = re.sub(r"\.(isra|constprop|part|cold)\.\d+", "", name)
    base = re.sub(r"\.(isra|constprop|part)\.\d+", "", base)
    if base in LIBM:
        return "sdk"
    obj = symmap.get(name, symmap.get(base))
    if obj is None:
        if base.startswith("fn_") or base.startswith("lbl_"):
            return "engine"
        return "other" if not re.match(r"^[a-z]\w*Func$|^[A-Z][a-z]+[A-Z]", base) else "engine?"
    if "/dolphin/mtx/" in obj or "psmtx_c" in obj:
        return "sdk"
    if "/port/src/gx/" in obj:
        return "gx"
    if "/port/src/audio" in obj or "/musyx/" in obj or "/src/msm/" in obj:
        return "audio"
    if "/game/src/" in obj:
        return "engine"
    return "other"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--thread", type=int, default=0)
    ap.add_argument("--map", default=os.path.join(os.path.dirname(__file__), "..", "build-ppc-darwin", "marioparty4.map"))
    ap.add_argument("--top", type=int, default=12)
    ap.add_argument("--audio-root", default="port_audio_tick")
    a = ap.parse_args()
    symmap = load_map(a.map)
    threads = parse(a.file)
    tname, nodes = threads[a.thread]
    total = nodes[0][1]
    # self per node, with the audio subtree and the idle waits split off
    stack = []
    selfc = []
    for i, (d, c, n) in enumerate(nodes):
        while stack and nodes[stack[-1]][0] >= d:
            stack.pop()
        path = [nodes[j][2] for j in stack]
        selfc.append([c, path])
        if stack:
            selfc[stack[-1]][0] -= c
        stack.append(i)
    by = defaultdict(lambda: defaultdict(int))
    for i, (d, c, n) in enumerate(nodes):
        s = selfc[i][0]
        if s <= 0:
            continue
        path = selfc[i][1]
        name = n.split("  (in ")[0].split(" + ")[0].strip()
        name = re.sub(r"\s+\(in .*$", "", name)
        if any(a.audio_root in p for p in path) or a.audio_root in name:
            k = "audio"
        elif re.search(r"semaphore_wait|mach_msg_trap|__semwait|_pthread_cond_wait|nanosleep|mach_wait_until|select\$", name):
            k = "wait"
        else:
            k = klass(name, symmap)
            if HELPER.match(name):
                # a helper is charged to the first real caller on its stack
                for p in reversed(path):
                    pn = re.sub(r"\s+\(in .*$", "", p.split("  (in ")[0].split(" + ")[0].strip())
                    if not HELPER.match(pn):
                        k = klass(pn, symmap)
                        name = name + " <" + pn
                        break
        by[k][name] += s
    print("%s: %d samples  (%s)" % (tname, total, a.file))
    order = ["engine", "engine?", "gx", "sdk", "audio", "other", "wait"]
    for k in order:
        if k not in by:
            continue
        t = sum(by[k].values())
        print("\n%-8s %6d  %5.1f%%" % (k, t, 100.0 * t / total))
        for n, c in sorted(by[k].items(), key=lambda x: -x[1])[: a.top]:
            print("    %-48s %6d  %5.1f%%" % (n[:48], c, 100.0 * c / total))


if __name__ == "__main__":
    main()
