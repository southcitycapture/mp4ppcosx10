#!/usr/bin/env python3
"""Generate `port/gen/sdk_stubs.c`: one loud stub per SDK symbol the game calls
that the port has not implemented by hand.

The list of symbols is not guessed -- it is the link's own undefined-symbol set
(`nm` over the compiled game objects, minus everything the port and libc
define), so nothing can be missed and nothing dead is generated.  The
signatures come from the SDK's own headers, so a stub that is later replaced by
a real implementation cannot silently disagree with its callers.

    gen_stubs.py --missing build-host/missing.txt \
                 --headers build-host/gen/include \
                 --out port/gen/sdk_stubs.c

Every stub prints its name the first time it is called (always with `--gxlog`
for the GX family) and counts its calls; `port_stub_report()` dumps the table,
which is what tells M1 exactly which SDK surface the boot actually touches.
"""
import argparse
import os
import re
import sys

# Return-value policy: a stub that returns a value returns this, because the
# boot sequence tests plenty of them.  Anything not listed returns zero.
RETURN_OVERRIDES = {
    "GXGetTexBufferSize": "0x1000",
}

DECL_RE = re.compile(
    r"^(?P<ret>[A-Za-z_][\w \t\*]*?[\w\*])[ \t]*"
    r"(?P<name>[A-Za-z_]\w*)[ \t]*\((?P<args>[^;{]*)\)[ \t]*;",
    re.S)

SKIP_LINE = ("#", "}", "typedef", "extern \"C\"")


def collect_decls(root):
    """Very small C declaration scraper: join continuation lines, keep the ones
    that look like a top-level function prototype."""
    decls = {}
    for dirpath, _, filenames in os.walk(root):
        for fn in filenames:
            if not fn.endswith(".h"):
                continue
            path = os.path.join(dirpath, fn)
            with open(path, errors="replace") as f:
                text = f.read()
            # strip comments
            text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
            text = re.sub(r"//[^\n]*", " ", text)
            depth = 0
            buf = ""
            for line in text.split("\n"):
                s = line.strip()
                if not s or s.startswith("#") or s.startswith('extern "C"'):
                    continue
                buf += " " + s
                depth += s.count("{") - s.count("}")
                if depth < 0:
                    depth = 0
                if depth > 0:
                    buf = ""
                    continue
                if ";" not in buf:
                    continue
                for stmt in buf.split(";"):
                    stmt = stmt.strip()
                    if not stmt:
                        continue
                    m = DECL_RE.match(stmt + ";")
                    if not m:
                        continue
                    ret = " ".join(m.group("ret").split())
                    if ret.startswith("extern "):
                        ret = ret[len("extern "):]
                    name = m.group("name")
                    args = " ".join(m.group("args").split()) or "void"
                    if ret in ("return", "typedef", "else", "static"):
                        continue
                    if ret.startswith("typedef"):
                        continue
                    decls.setdefault(name, (ret, args))
                buf = ""
    return decls


TYPE_WORDS = {
    "void", "char", "short", "int", "long", "float", "double", "signed",
    "unsigned", "const", "volatile", "struct", "union", "enum", "_Bool", "bool",
    "s8", "s16", "s32", "s64", "u8", "u16", "u32", "u64", "f32", "f64", "BOOL",
}


def name_params(args):
    """Give every unnamed parameter a name: an unnamed parameter in a
    *definition* is a C23 extension and GCC 14 rejects it under -std=gnu11."""
    if args.strip() in ("", "void"):
        return "void"
    out = []
    for i, a in enumerate(args.split(",")):
        a = a.strip()
        if not a or a == "...":
            out.append(a or "void")
            continue
        if a.endswith("]"):  # array parameter, e.g. f32 mtx[4][4]
            head = a[:a.index("[")].strip()
            tail = a[a.index("["):]
            toks = head.replace("*", " * ").split()
            if not toks or toks[-1] in TYPE_WORDS or toks[-1] == "*":
                a = head + " _a%d" % i + tail
            out.append(a)
            continue
        toks = a.replace("*", " * ").split()
        if not toks or toks[-1] in TYPE_WORDS or toks[-1] == "*":
            a = a + " _a%d" % i
        out.append(a)
    return ", ".join(out)


def zero_for(ret):
    r = ret.replace("const", "").strip()
    if r == "void":
        return None
    if "*" in r:
        return "NULL"
    if r in ("f32", "float", "f64", "double"):
        return "0.0f" if r in ("f32", "float") else "0.0"
    return "0"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--missing", required=True)
    ap.add_argument("--headers", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--out-untyped", required=True)
    ap.add_argument("--exclude", default="", help="file of symbols to skip")
    args = ap.parse_args()

    decls = collect_decls(args.headers)
    skip = set()
    if args.exclude and os.path.exists(args.exclude):
        with open(args.exclude) as f:
            for line in f:
                line = line.split("#")[0].strip()
                if line:
                    skip.add(line)

    wanted = []
    with open(args.missing) as f:
        for line in f:
            sym = line.strip()
            if not sym or sym in skip:
                continue
            wanted.append(sym)

    known, unknown = [], []
    for sym in wanted:
        (known if sym in decls else unknown).append(sym)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w") as o:
        o.write("/* GENERATED by port/tools/gen_stubs.py -- do not edit.\n"
                " * One loud stub per SDK symbol the game calls that the port has not\n"
                " * implemented yet.  %d with a real signature from the SDK headers,\n"
                " * %d without.\n */\n" % (len(known), len(unknown)))
        o.write('#include "port.h"\n')
        o.write("#include <dolphin.h>\n#include <dolphin/gx.h>\n#include <dolphin/card.h>\n"
                "#include <dolphin/ai.h>\n#include <dolphin/ar.h>\n#include <dolphin/arq.h>\n"
                "#include <dolphin/dsp.h>\n#include <dolphin/thp.h>\n"
                "#include <dolphin/demo/DEMOStats.h>\n"
                "#include <stddef.h>\n\n")
        for sym in known:
            ret, argsig = decls[sym]
            z = zero_for(ret)
            o.write("%s %s(%s) {\n" % (ret, sym, name_params(argsig)))
            o.write('    port_stub("%s");\n' % sym)
            if z is not None:
                o.write("    return (%s)%s;\n" % (ret, RETURN_OVERRIDES.get(sym, z)))
            o.write("}\n")
    with open(args.out_untyped, "w") as o:
        o.write("/* GENERATED by port/tools/gen_stubs.py -- do not edit.\n"
                " * %d SDK symbols with no prototype anywhere in the headers.  Declared\n"
                " * with no parameters and compiled without the SDK headers, so there is\n"
                " * nothing to disagree with: the caller passes its arguments in registers\n"
                " * and the stub ignores them, which is safe on both ABIs.\n */\n"
                % len(unknown))
        o.write('#include "port.h"\n\n')
        for sym in unknown:
            o.write("long %s(void);\nlong %s(void) { port_stub(\"%s\"); return 0; }\n"
                    % (sym, sym, sym))
    print("gen_stubs: %d stubs (%d typed, %d untyped) -> %s"
          % (len(wanted), len(known), len(unknown), args.out))


if __name__ == "__main__":
    main()
