#!/usr/bin/env python3
"""Widen the game's pointer-through-u32 casts, in the mirror only.

The decomp is 32-bit code: it casts pointers to `u32` freely -- `(void *)((u32)
base + ofs)` all over hsfload.c's file-format fixups, msmmem.c's allocator,
sprman.c, saveload.c.  On the GameCube and on the G4 that is exactly right and
`uintptr_t` is the same 32-bit type, so this rewrite changes nothing at all for
the real target.  On the 64-bit development host it is the difference between
booting and a segfault.

Rather than guess which casts are pointer casts, ask the compiler: build every
mirrored source with `-Wpointer-to-int-cast -Wvoid-pointer-to-int-cast` and
rewrite exactly the casts it flags.  Nothing else is touched, so a cast the game
*means* to truncate is left alone.

    widen_ptr_casts.py --gen build-host/gen --cc clang --cflags "..."
"""
import argparse
import os
import re
import subprocess
import sys

WARN_RE = re.compile(r"^(?P<file>[^:]+):(?P<line>\d+):(?P<col>\d+): warning: "
                     r"cast to smaller integer type '(?P<type>[us]\d+)'")
WIDEN = {"u32": "uintptr_t", "s32": "intptr_t", "u64": "uintptr_t", "s64": "intptr_t"}


def sources(gen):
    out = []
    for dirpath, _, filenames in os.walk(os.path.join(gen, "src")):
        for fn in filenames:
            if fn.endswith(".c"):
                out.append(os.path.join(dirpath, fn))
    return sorted(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gen", required=True)
    ap.add_argument("--cc", required=True)
    ap.add_argument("--cflags", required=True)
    ap.add_argument("--jobs", type=int, default=8)
    args = ap.parse_args()

    flags = args.cflags.split() + [
        "-fsyntax-only", "-Wno-everything",
        "-Wpointer-to-int-cast", "-Wvoid-pointer-to-int-cast",
    ]

    total = 0
    narrow = set()
    inmacro = set()
    for rounds in range(4):
        hits = {}
        procs = []
        srcs = sources(args.gen)
        for i in range(0, len(srcs), args.jobs):
            batch = srcs[i:i + args.jobs]
            procs = [(s, subprocess.Popen([args.cc, s] + flags, stdout=subprocess.DEVNULL,
                                          stderr=subprocess.PIPE, text=True)) for s in batch]
            for s, p in procs:
                _, err = p.communicate()
                for line in err.split("\n"):
                    m = WARN_RE.match(line)
                    if m:
                        if m.group("type") not in WIDEN:
                            # e.g. board code packing a message id into an s16;
                            # a deliberate truncation, not a pointer cast.
                            narrow.add("%s:%s (%s)" % (m.group("file"), m.group("line"),
                                                       m.group("type")))
                            continue
                        hits.setdefault(m.group("file"), []).append(
                            (int(m.group("line")), int(m.group("col")), m.group("type")))
        if not hits:
            break
        n = 0
        for path, sites in hits.items():
            with open(path) as f:
                lines = f.read().split("\n")
            # right to left within each line so earlier columns stay valid
            for ln, col, ty in sorted(set(sites), key=lambda s: (-s[0], -s[1])):
                text = lines[ln - 1]
                token = "(%s)" % ty
                at = text.find(token, max(0, col - 1 - len(token)))
                if at < 0 or at > col:
                    at = text.rfind(token, 0, col + len(token))
                if at < 0:
                    # the cast is inside a macro (window.h's MAKE_MESSID_PTR,
                    # which packs either a message id or a string pointer into
                    # a u32); the rewrite cannot reach it from the call site.
                    inmacro.add("%s:%d" % (path, ln))
                    continue
                lines[ln - 1] = text[:at] + "(" + WIDEN[ty] + ")" + text[at + len(token):]
                n += 1
            with open(path, "w") as f:
                f.write("\n".join(lines))
        total += n
        if n == 0:
            break
    for n in sorted(narrow):
        print("widen: left alone (deliberate narrowing): %s" % n, file=sys.stderr)
    if inmacro:
        print("widen: %d sites are casts inside a macro and were left alone "
              "(window.h MAKE_MESSID_PTR); see port/docs/PLAN.md M1 log"
              % len(inmacro), file=sys.stderr)
    print("widen: %d pointer casts widened to uintptr_t in the mirror" % total)


if __name__ == "__main__":
    main()
