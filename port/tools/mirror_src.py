#!/usr/bin/env python3
"""Build the port's source mirror.

The decomp's own files are never edited.  Everything the port needs to change
in the game sources or in the SDK headers goes through this filter, so
`git diff` against upstream stays empty and an upstream merge costs nothing.

Three transformations, in order:

1. **exact-text patches** from `port/patches.txt`, one per line, tab separated

       [target:]relative/path<TAB>old text<TAB>new text

   `\\n` and `\\t` are unescaped in both texts, so a patch may span lines.  Each
   patch must match its file exactly once, or the build fails -- that is the
   point: an upstream edit that moves the text breaks the build loudly instead
   of silently doing nothing.  An optional `host:` / `ppc:` prefix restricts a
   patch to one target.

2. **Metrowerks assembly stripping** (`.c` sources only).  Two forms:
   whole-function `asm <type> name(args) { ... }` and a statement-level
   `asm { ... }` block inside a C function.  Both are deleted; the port
   supplies replacements (`port/src/os/jmp_*.s`, `port/src/os/psmtx_c.c`).
   Every removal is reported to stdout so nothing disappears silently.

3. **whole-file overrides** from `port/include/override/`, copied over the
   mirrored include tree last (currently only `dolphin/os/OSFastCast.h`, whose
   paired-single fast casts have no 7450 equivalent).

The C-library shims the decomp ships for Metrowerks (`include/string.h` and
friends, whose prototypes do not match a real libc) are left out of the mirror
so the host's own headers are found instead.

    mirror_src.py --out build-host/gen --target host
"""
import argparse
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
PORT = os.path.abspath(os.path.join(HERE, ".."))

# Metrowerks' own C library headers, replaced by the host's.
LIBC_SHIMS = {
    "stdint.h", "stddef.h", "stdio.h", "stdlib.h", "string.h",
    "float.h", "stdarg.h", "math.h",
}

# Game translation units the port compiles: exactly the DOL's own code, plus
# the pure-C half of the SDK's matrix library.  Mirrors configure.py's `Game`,
# `libhu`, `msm` and `mtx` libraries; `psmtx.c` is 100% paired singles with no
# C equivalent and is replaced wholesale by port/src/os/psmtx_c.c.
SRC_GLOBS = ["src/game/*.c", "src/game/board/*.c", "src/msm/*.c", "src/libhu/*.c"]
MTX_SRCS = ["src/dolphin/mtx/mtx.c", "src/dolphin/mtx/mtxvec.c",
            "src/dolphin/mtx/mtx44.c", "src/dolphin/mtx/vec.c",
            "src/dolphin/mtx/quat.c"]


ESCAPES = {"n": "\n", "t": "\t", "\\": "\\"}


def unescape(s):
    """`\\n` and `\\t` become a newline and a tab; `\\\\` is a literal backslash, so
    a patch can still match C source that contains "\\n" inside a string."""
    out = []
    i = 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s) and s[i + 1] in ESCAPES:
            out.append(ESCAPES[s[i + 1]])
            i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


def load_patches(path):
    out = []
    if not os.path.exists(path):
        return out
    with open(path) as f:
        for lineno, line in enumerate(f, 1):
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parts = line.rstrip("\n").split("\t")
            if len(parts) != 3:
                raise SystemExit("%s:%d: expected 3 tab-separated fields, got %d"
                                 % (path, lineno, len(parts)))
            head = parts[0]
            if ":" in head:
                spec, rel = head.split(":", 1)
            else:
                spec, rel = "", head
            target, count = "", 1
            for tok in spec.split(",") if spec else []:
                if tok.startswith("x"):
                    count = int(tok[1:])
                else:
                    target = tok
            out.append((target, rel, unescape(parts[1]), unescape(parts[2]), lineno, count))
    return out




def strip_mwasm(text, rel, log):
    """Delete every top-level definition that contains Metrowerks assembly.

    Two forms appear in this tree: whole-function `asm <type> name(args) {...}`
    and a C function with a statement-level `asm { ... }` block in its body.
    Both are dropped *entirely* -- stripping only the asm out of the second
    form would leave a silently wrong C function behind (`PSMTXIdentity` that
    identities nothing).  The port supplies the replacements, and if it forgets
    one the link fails loudly.
    """
    lines = text.split("\n")
    out = []
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]
        # a top-level definition begins at column 0 with an identifier or `asm`
        if not line or line[0] in " \t#/*}" or line.startswith("//"):
            out.append(line)
            i += 1
            continue
        # gather until braces balance (or a `;` at depth 0 -> a declaration)
        j = i
        depth = 0
        seen_brace = False
        while j < n:
            depth += lines[j].count("{") - lines[j].count("}")
            if "{" in lines[j]:
                seen_brace = True
            if seen_brace and depth <= 0:
                j += 1
                break
            if not seen_brace and lines[j].rstrip().endswith(";"):
                j += 1
                break
            j += 1
        chunk = lines[i:j]
        # A column-0 `inline` definition (11 of them, in hsfman.c and
        # hsfdraw.c) emits no out-of-line copy under C99 rules, but Metrowerks
        # emitted one and other translation units call it.  Drop the keyword.
        if chunk[0].startswith("inline ") and seen_brace:
            log.append("%s:%d: dropped `inline` from %s"
                       % (rel, i + 1, chunk[0].strip()[:70]))
            chunk[0] = chunk[0][len("inline "):]
        head = chunk[0].strip()
        has_asm = head.startswith("asm ") or any(
            l.strip() in ("asm {", "asm{", "asm") for l in chunk)
        if has_asm and seen_brace:
            log.append("%s:%d: dropped %d lines: %s" % (rel, i + 1, len(chunk), head[:70]))
        else:
            out.extend(chunk)
        i = j
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, help="mirror root, e.g. build-host/gen")
    ap.add_argument("--target", default="", help="host | ppc")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    patches = load_patches(os.path.join(PORT, "patches.txt"))
    used = set()
    log = []

    def emit(rel, dst):
        with open(os.path.join(ROOT, rel), "rb") as f:
            text = f.read().decode("utf-8", "surrogateescape")
        for k, (target, path, old, new, lineno, count) in enumerate(patches):
            if path != rel or (target and target != args.target):
                continue
            n = text.count(old)
            if n != count:
                raise SystemExit("patches.txt:%d: %s: expected exactly %d match(es) "
                                 "for %r, found %d" % (lineno, path, count, old, n))
            text = text.replace(old, new)
            used.add(k)
        if rel.endswith(".c"):
            text = strip_mwasm(text, rel, log)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(text.encode("utf-8", "surrogateescape"))

    out = os.path.abspath(args.out)
    shutil.rmtree(out, ignore_errors=True)

    # the SDK / game include tree
    for dirpath, dirnames, filenames in os.walk(os.path.join(ROOT, "include")):
        for fn in filenames:
            if not fn.endswith(".h"):
                continue
            src = os.path.join(dirpath, fn)
            rel = os.path.relpath(src, ROOT).replace(os.sep, "/")
            if rel.count("/") == 1 and fn in LIBC_SHIMS:
                continue
            emit(rel, os.path.join(out, rel))

    # the game sources
    import glob
    srcs = []
    for g in SRC_GLOBS:
        srcs.extend(sorted(glob.glob(os.path.join(ROOT, g))))
    srcs.extend(os.path.join(ROOT, s) for s in MTX_SRCS)
    for src in srcs:
        rel = os.path.relpath(src, ROOT).replace(os.sep, "/")
        emit(rel, os.path.join(out, rel))

    # whole-file header overrides, last
    override = os.path.join(PORT, "include", "override")
    if os.path.isdir(override):
        for dirpath, _, filenames in os.walk(override):
            for fn in filenames:
                src = os.path.join(dirpath, fn)
                rel = os.path.relpath(src, override)
                dst = os.path.join(out, "include", rel)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copyfile(src, dst)
                log.append("include/%s: replaced wholesale by port/include/override" % rel)

    unused = [p for k, p in enumerate(patches) if k not in used]
    if unused:
        for target, path, old, _, lineno, _count in unused:
            if target and target != args.target:
                continue
            raise SystemExit("patches.txt:%d: %s never mirrored (typo in the path?)"
                             % (lineno, path))

    if not args.quiet:
        for line in log:
            print("mirror: " + line)
    print("mirror: %d headers + %d sources -> %s" % (
        sum(len(f) for _, _, f in os.walk(os.path.join(out, "include"))), len(srcs), args.out))


if __name__ == "__main__":
    main()
