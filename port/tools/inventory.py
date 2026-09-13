#!/usr/bin/env python3
"""
inventory.py -- static inventory of the Mario Party 4 decomp, for the
Power Mac G4 native port.

Walks the decomp's C sources and answers the questions the port plan needs:

  * how big is each area of the tree (DOL game code, each REL, the SDK,
    MusyX, the MSL/Runtime support code);
  * which Dolphin SDK entry points are called, how often, and from where --
    this is the exact surface the port layer has to provide;
  * how hard the GX usage is: TEV stage counts, indirect texturing, texgens,
    lighting channels, fog, Z-textures, EFB copies, texture formats, TLUTs,
    display lists, vertex descriptor churn;
  * where the code will fight a GCC/PowerPC-Darwin cross build: inline asm,
    Metrowerks `#pragma`s, `__declspec`, paired-single intrinsics, fast-casts.

It reads nothing but the decomp's own sources, so it can be re-run after any
upstream merge:

    python3 port/tools/inventory.py                 # writes port/docs/inventory.md
    python3 port/tools/inventory.py --stdout        # print instead
    python3 port/tools/inventory.py --repo /path    # other checkout

Counts are lexical (regex over source text), not semantic: a call inside a
`#if VERSION_PAL` block still counts, and a call written through a function
pointer does not.  That is fine for sizing work; it is not a linker.
"""

from __future__ import annotations

import argparse
import collections
import os
import re
import sys
from pathlib import Path

# --------------------------------------------------------------------------
# Areas of the tree.  Order matters: first match wins.
# --------------------------------------------------------------------------

AREAS = [
    ("sdk/dolphin", "src/dolphin"),
    ("sdk/musyx", "extern/musyx/src"),
    ("game/msm", "src/msm"),
    ("game/libhu", "src/libhu"),
    ("game/board", "src/game/board"),
    ("game/dol", "src/game"),
    ("rel", "src/REL"),
    ("support/msl", "src/MSL_C.PPCEABI.bare.H"),
    ("support/runtime", "src/Runtime.PPCEABI.H"),
    ("support/trk", "src/TRK_MINNOW_DOLPHIN"),
    ("support/exi2", "src/OdemuExi2"),
    ("support/amcstubs", "src/amcstubs"),
    ("support/odenotstub", "src/odenotstub"),
]

# SDK families: prefix regex -> family name.  Anchored on a word boundary and
# followed by an open paren so we only see call/definition sites.
FAMILIES = [
    ("GX", r"GX[A-Z][A-Za-z0-9_]*"),
    ("OS", r"OS[A-Z][A-Za-z0-9_]*"),
    ("DVD", r"DVD[A-Z][A-Za-z0-9_]*"),
    ("CARD", r"CARD[A-Z][A-Za-z0-9_]*"),
    ("PAD", r"PAD[A-Z][A-Za-z0-9_]*"),
    ("VI", r"VI[A-Z][A-Za-z0-9_]*"),
    ("AI", r"AI[A-Z][A-Za-z0-9_]*"),
    ("AR", r"AR(?:Q)?[A-Z][A-Za-z0-9_]*"),
    ("DSP", r"DSP[A-Z][A-Za-z0-9_]*"),
    ("EXI", r"EXI[A-Z][A-Za-z0-9_]*"),
    ("SI", r"SI[A-Z][A-Za-z0-9_]*"),
    ("THP", r"THP[A-Z][A-Za-z0-9_]*"),
    ("DEMO", r"DEMO[A-Z][A-Za-z0-9_]*"),
    ("MTX", r"(?:PSMTX|C_MTX|MTX|PSVEC|C_VEC|VEC|Mtx)[A-Za-z0-9_]*"),
    ("DC/IC cache", r"(?:DC|IC)(?:Store|Flush|Invalidate|Zero|Touch|Block|Enable|Disable|Freeze|Unfreeze)[A-Za-z0-9_]*"),
    ("MSM (game audio mgr)", r"msm[A-Z][A-Za-z0-9_]*"),
    ("snd/MusyX", r"snd[A-Z][A-Za-z0-9_]*"),
]

# Portability hazards for a GCC / PowerPC-Darwin cross build.
HAZARDS = [
    ("inline asm block  `asm {`", r"\basm\s*\{"),
    ("asm function      `asm <type>`", r"^\s*asm\s+[A-Za-z_][A-Za-z0-9_ \*]*\("),
    ("gcc inline asm", r"__asm__|asm\s+volatile"),
    ("`__declspec`", r"__declspec"),
    ("`#pragma`", r"^\s*#\s*pragma"),
    ("`__attribute__`", r"__attribute__"),
    ("`register` storage class", r"\bregister\s+[A-Za-z_]"),
    ("paired-single intrinsic", r"\b(?:ps_[a-z]+|psq_l[ux]?|psq_st[ux]?|__fres|__frsqrte)\b"),
    ("OS fast-cast (`OSf32to*`)", r"\bOSf32to[su](?:8|16|32)\b|OSInitFastCast"),
    ("MW intrinsic (`__cntlzw` etc)", r"__cntlzw|__rlwinm|__mulhw|__abs|__fabs|__fnabs|__fsel|__sqrt"),
    ("`ATTRIBUTE_ALIGN`", r"ATTRIBUTE_ALIGN"),
    ("physical/cached addr macro", r"OSPhysicalTo|OSCachedTo|OSUncachedTo"),
    ("MMIO / SPR access", r"\bmfspr|\bmtspr|\bmfmsr|\bmtmsr|0xCC00|__OSPhysical"),
]

# GX detail probes: label -> regex over the whole file text.
GX_PROBES = [
    ("TEV stage cap set        GXSetNumTevStages", r"\bGXSetNumTevStages\s*\("),
    ("TEV colour input         GXSetTevColorIn", r"\bGXSetTevColorIn\s*\("),
    ("TEV alpha input          GXSetTevAlphaIn", r"\bGXSetTevAlphaIn\s*\("),
    ("TEV canned op            GXSetTevOp", r"\bGXSetTevOp\s*\("),
    ("TEV konst colour         GXSetTevColor", r"\bGXSetTevColor(?:S10)?\s*\("),
    ("TEV konst select         GXSetTevK*Sel", r"\bGXSetTevK(?:Color|Alpha)Sel\s*\("),
    ("TEV swap                 GXSetTevSwapMode", r"\bGXSetTevSwapMode(?:Table)?\s*\("),
    ("TEV order                GXSetTevOrder", r"\bGXSetTevOrder\s*\("),
    ("indirect: stage count    GXSetNumIndStages", r"\bGXSetNumIndStages\s*\("),
    ("indirect: order          GXSetIndTexOrder", r"\bGXSetIndTexOrder\s*\("),
    ("indirect: matrix         GXSetIndTexMtx", r"\bGXSetIndTexMtx\s*\("),
    ("indirect: coord scale    GXSetIndTexCoordScale", r"\bGXSetIndTexCoordScale\s*\("),
    ("indirect: warp           GXSetTevIndWarp", r"\bGXSetTevIndWarp\s*\("),
    ("indirect: generic        GXSetTevIndirect", r"\bGXSetTevIndirect\s*\("),
    ("indirect: tile/bump      GXSetTevInd(Tile|Bump)", r"\bGXSetTevInd(?:Tile|BumpST|BumpXYZ)\s*\("),
    ("indirect: off            GXSetTevDirect", r"\bGXSetTevDirect\s*\("),
    ("texgen count             GXSetNumTexGens", r"\bGXSetNumTexGens\s*\("),
    ("texgen setup             GXSetTexCoordGen*", r"\bGXSetTexCoordGen2?\s*\("),
    ("colour channels          GXSetNumChans", r"\bGXSetNumChans\s*\("),
    ("channel control          GXSetChanCtrl", r"\bGXSetChanCtrl\s*\("),
    ("light object             GXInitLight*", r"\bGXInitLight[A-Za-z]*\s*\("),
    ("light load               GXLoadLightObjImm", r"\bGXLoadLightObj(?:Imm)?\s*\("),
    ("fog                      GXSetFog*", r"\bGXSetFog[A-Za-z]*\s*\("),
    ("Z texture                GXSetZTexture", r"\bGXSetZTexture\s*\("),
    ("Z mode                   GXSetZMode", r"\bGXSetZMode\s*\("),
    ("Z compare location       GXSetZCompLoc", r"\bGXSetZCompLoc\s*\("),
    ("alpha compare            GXSetAlphaCompare", r"\bGXSetAlphaCompare\s*\("),
    ("blend                    GXSetBlendMode", r"\bGXSetBlendMode\s*\("),
    ("dither                   GXSetDither", r"\bGXSetDither\s*\("),
    ("dst alpha                GXSetDstAlpha", r"\bGXSetDstAlpha\s*\("),
    ("EFB->texture copy        GXCopyTex", r"\bGXCopyTex\s*\("),
    ("EFB copy src/dst         GXSetTexCopySrc/Dst", r"\bGXSetTexCopy(?:Src|Dst)\s*\("),
    ("EFB->XFB copy            GXCopyDisp", r"\bGXCopyDisp\s*\("),
    ("display list build       GXBeginDisplayList", r"\bGXBeginDisplayList\s*\("),
    ("display list call        GXCallDisplayList", r"\bGXCallDisplayList\s*\("),
    ("vertex desc              GXSetVtxDesc*", r"\bGXSetVtxDescv?\s*\(|\bGXClearVtxDesc\s*\("),
    ("vertex format            GXSetVtxAttrFmt*", r"\bGXSetVtxAttrFmtv?\s*\("),
    ("vertex array base        GXSetArray", r"\bGXSetArray\s*\("),
    ("immediate draw           GXBegin", r"\bGXBegin\s*\("),
    ("pos matrix load          GXLoadPosMtxImm", r"\bGXLoadPosMtxImm\s*\("),
    ("nrm matrix load          GXLoadNrmMtxImm", r"\bGXLoadNrmMtxImm\s*\("),
    ("tex matrix load          GXLoadTexMtxImm", r"\bGXLoadTexMtxImm\s*\("),
    ("per-vertex matrix index  GX_VA_PNMTXIDX", r"GX_VA_(?:PN|TEX\d)MTXIDX"),
    ("texture object init      GXInitTexObj*", r"\bGXInitTexObj[A-Za-z]*\s*\("),
    ("TLUT                     GXInitTlutObj/GXLoadTlut", r"\bGXInitTlutObj\s*\(|\bGXLoadTlut\s*\(", ),
    ("scissor                  GXSetScissor", r"\bGXSetScissor\s*\(", ),
    ("cull                     GXSetCullMode", r"\bGXSetCullMode\s*\(", ),
    ("projection               GXSetProjection", r"\bGXSetProjection\s*\(", ),
    ("viewport                 GXSetViewport*", r"\bGXSetViewport[A-Za-z]*\s*\(", ),
    ("perf metrics             GX*Metric", r"\bGX(?:Set|Read|Clear)[A-Za-z]*Metric\s*\(", ),
]

ENUM_PROBES = [
    ("texture format", r"GX_TF_[A-Z0-9_]+"),
    ("copy texture format", r"GX_CTF_[A-Z0-9_]+"),
    ("TLUT format", r"GX_TL_[A-Z0-9_]+"),
    ("TEV stage id", r"GX_TEVSTAGE\d+"),
    ("indirect stage id", r"GX_INDTEXSTAGE\d+"),
    ("texmap id", r"GX_TEXMAP\d"),
    ("texcoord id", r"GX_TEXCOORD\d"),
    ("blend factor", r"GX_BL_[A-Z0-9_]+"),
    ("compare func", r"GX_(?:NEVER|LESS|EQUAL|LEQUAL|GREATER|NEQUAL|GEQUAL|ALWAYS)\b"),
    ("primitive", r"GX_(?:POINTS|LINES|LINESTRIP|TRIANGLES|TRIANGLESTRIP|TRIANGLEFAN|QUADS)\b"),
    ("vertex attr", r"GX_VA_[A-Z0-9_]+"),
    ("vertex component fmt", r"GX_(?:U8|S8|U16|S16|F32|RGB565|RGB8|RGBX8|RGBA4|RGBA6|RGBA8)\b"),
    ("vertex index size", r"GX_(?:DIRECT|INDEX8|INDEX16|NONE)\b"),
]

C_EXT = {".c"}
H_EXT = {".h"}


def iter_files(repo: Path, rel: str):
    root = repo / rel
    if not root.is_dir():
        return
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if d != ".git")
        for name in sorted(filenames):
            ext = os.path.splitext(name)[1]
            if ext in C_EXT or ext in H_EXT:
                yield Path(dirpath) / name


def area_of(repo: Path, path: Path) -> str:
    rel = str(path.relative_to(repo))
    for name, prefix in AREAS:
        if rel == prefix or rel.startswith(prefix + os.sep):
            if name == "rel":
                parts = rel.split(os.sep)
                return "rel/" + (parts[2] if len(parts) > 2 else "?")
            return name
    return "other"


def strip_comments(text: str) -> str:
    """Cheap comment/string stripper so counts are not inflated by OSReport
    format strings and by commented-out code."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            out.append(" ")
        elif c == '"':
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == '"':
                    break
                j += 1
            out.append('""')
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def main() -> int:
    here = Path(__file__).resolve()
    default_repo = here.parent.parent.parent
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", default=str(default_repo), help="decomp checkout root")
    ap.add_argument("--out", default=None, help="output markdown (default port/docs/inventory.md)")
    ap.add_argument("--stdout", action="store_true", help="print instead of writing")
    ap.add_argument("--symbols", action="store_true",
                    help="instead of the report, print a TSV of every SDK symbol the "
                         "game side calls: family, symbol, game sites, REL sites. "
                         "This is the input the stub generator consumes.")
    args = ap.parse_args()

    repo = Path(args.repo).resolve()
    out_path = Path(args.out) if args.out else repo / "port" / "docs" / "inventory.md"

    fam_res = [(name, re.compile(r"\b(" + pat + r")\s*\(")) for name, pat in FAMILIES]
    haz_res = [(name, re.compile(pat, re.M)) for name, pat in HAZARDS]
    gx_res = [(name, re.compile(pat)) for name, pat, *_ in GX_PROBES]
    enum_res = [(name, re.compile(pat)) for name, pat in ENUM_PROBES]

    # area -> stats
    area_files = collections.Counter()
    area_lines = collections.Counter()
    # (area, family) -> count ; (family, symbol) -> Counter(area)
    area_family = collections.Counter()
    symbol_area = collections.defaultdict(collections.Counter)
    symbol_family = {}
    hazard_hits = collections.defaultdict(list)      # hazard -> [(file, count)]
    gx_hits = collections.Counter()                  # probe -> count (game+rel only)
    gx_hits_all = collections.Counter()
    enum_hits = collections.defaultdict(collections.Counter)
    numtev_args = collections.Counter()
    numchan_args = collections.Counter()
    numtexgen_args = collections.Counter()
    numind_args = collections.Counter()
    rel_lines = collections.Counter()
    rel_files = collections.Counter()

    numtev_re = re.compile(r"\bGXSetNumTevStages\s*\(\s*([^)]*?)\s*\)")
    numchan_re = re.compile(r"\bGXSetNumChans\s*\(\s*([^)]*?)\s*\)")
    numtexgen_re = re.compile(r"\bGXSetNumTexGens\s*\(\s*([^)]*?)\s*\)")
    numind_re = re.compile(r"\bGXSetNumIndStages\s*\(\s*([^)]*?)\s*\)")

    seen_paths = []
    for _, prefix in AREAS:
        for path in iter_files(repo, prefix):
            seen_paths.append(path)

    for path in seen_paths:
        area = area_of(repo, path)
        try:
            raw = path.read_text(errors="replace")
        except OSError:
            continue
        nlines = raw.count("\n") + 1
        is_c = path.suffix == ".c"
        area_files[area] += 1
        area_lines[area] += nlines
        if area.startswith("rel/"):
            rel_files[area[4:]] += 1
            rel_lines[area[4:]] += nlines
        if not is_c:
            continue
        text = strip_comments(raw)

        for fam, rx in fam_res:
            for m in rx.finditer(text):
                sym = m.group(1)
                area_family[(area, fam)] += 1
                symbol_area[sym][area] += 1
                symbol_family[sym] = fam

        for name, rx in haz_res:
            k = len(rx.findall(text))
            if k:
                hazard_hits[name].append((str(path.relative_to(repo)), k))

        gameish = area.startswith("game/") or area.startswith("rel/")
        for name, rx in gx_res:
            k = len(rx.findall(text))
            if k:
                gx_hits_all[name] += k
                if gameish:
                    gx_hits[name] += k
        for name, rx in enum_res:
            if gameish:
                for m in rx.finditer(text):
                    enum_hits[name][m.group(0)] += 1
        if gameish:
            for m in numtev_re.finditer(text):
                numtev_args[m.group(1)] += 1
            for m in numchan_re.finditer(text):
                numchan_args[m.group(1)] += 1
            for m in numtexgen_re.finditer(text):
                numtexgen_args[m.group(1)] += 1
            for m in numind_re.finditer(text):
                numind_args[m.group(1)] += 1

    # --------------------------------------------------------------- symbols
    if args.symbols:
        rows = []
        for sym, per_area in symbol_area.items():
            game = sum(v for a, v in per_area.items() if a.startswith("game/"))
            rel = sum(v for a, v in per_area.items() if a.startswith("rel/"))
            if game + rel:
                rows.append((symbol_family.get(sym, "?"), sym, game, rel))
        rows.sort(key=lambda r: (r[0], -(r[2] + r[3]), r[1]))
        print("family\tsymbol\tgame_sites\trel_sites")
        for fam, sym, game, rel in rows:
            print(f"{fam}\t{sym}\t{game}\t{rel}")
        return 0

    # ---------------------------------------------------------------- render
    o = []
    w = o.append
    w("# Mario Party 4 -- SDK and GX inventory")
    w("")
    w("Generated by `port/tools/inventory.py`. Re-run after any upstream merge:")
    w("")
    w("```sh")
    w("python3 port/tools/inventory.py")
    w("```")
    w("")
    w("Counts are lexical: a regex over comment-stripped, string-stripped C source.")
    w("They count *sites*, not dynamic calls, and they include sites inside")
    w("`#if VERSION_*` blocks for versions we do not build. `sdk/*` rows include the")
    w("SDK's own definitions and internal calls, so read them as \"how much code is")
    w("there\", not \"how much the game asks for\".")
    w("")

    # areas
    w("## 1. Size of the tree, by area")
    w("")
    w("| area | files (.c/.h) | lines |")
    w("|---|---:|---:|")
    rel_total_f = rel_total_l = 0
    for area in sorted(area_files):
        if area.startswith("rel/"):
            rel_total_f += area_files[area]
            rel_total_l += area_lines[area]
            continue
        w(f"| `{area}` | {area_files[area]} | {area_lines[area]:,} |")
    w(f"| `rel/*` ({len(rel_files)} modules, aggregated) | {rel_total_f} | {rel_total_l:,} |")
    w(f"| **total** | **{sum(area_files.values())}** | **{sum(area_lines.values()):,}** |")
    w("")

    # rel table
    w("### 1.1 REL modules")
    w("")
    w("| module | files | lines |")
    w("|---|---:|---:|")
    for mod, _ in rel_lines.most_common():
        w(f"| `{mod}` | {rel_files[mod]} | {rel_lines[mod]:,} |")
    w("")

    # families
    w("## 2. Dolphin SDK call sites, by family and area")
    w("")
    fams = [f for f, _ in FAMILIES]
    areas_sorted = sorted({a for a, _ in area_family})
    game_areas = [a for a in areas_sorted if a.startswith("game/")]
    rel_areas = [a for a in areas_sorted if a.startswith("rel/")]
    other_areas = [a for a in areas_sorted if not a.startswith(("game/", "rel/"))]
    w("| family | game/dol | game/board | game/msm | game/libhu | all RELs | sdk/dolphin | sdk/musyx | support/* |")
    w("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for fam in fams:
        row = [
            area_family[("game/dol", fam)],
            area_family[("game/board", fam)],
            area_family[("game/msm", fam)],
            area_family[("game/libhu", fam)],
            sum(area_family[(a, fam)] for a in rel_areas),
            area_family[("sdk/dolphin", fam)],
            area_family[("sdk/musyx", fam)],
            sum(area_family[(a, fam)] for a in other_areas if a.startswith("support/")),
        ]
        if not any(row):
            continue
        w("| " + fam + " | " + " | ".join(str(x) for x in row) + " |")
    w("")

    # full symbol table for the port surface
    w("## 3. The port surface: every SDK symbol the game side calls")
    w("")
    w("\"game side\" = `game/*` + `rel/*` (the code we compile and keep).")
    w("Anything with a non-zero count here has to exist -- really or as a stub --")
    w("before the native binary links.")
    w("")
    for fam in fams:
        rows = []
        for sym, per_area in symbol_area.items():
            if symbol_family.get(sym) != fam:
                continue
            game = sum(v for a, v in per_area.items() if a.startswith("game/"))
            rel = sum(v for a, v in per_area.items() if a.startswith("rel/"))
            if game + rel == 0:
                continue
            rows.append((game + rel, game, rel, sym))
        if not rows:
            continue
        rows.sort(key=lambda r: (-r[0], r[3]))
        w(f"### 3.{fams.index(fam)+1} {fam} -- {len(rows)} distinct symbols, "
          f"{sum(r[0] for r in rows)} call sites")
        w("")
        w("| symbol | total | game | RELs |")
        w("|---|---:|---:|---:|")
        for total, game, rel, sym in rows:
            w(f"| `{sym}` | {total} | {game} | {rel} |")
        w("")

    # GX detail
    w("## 4. GX feature probes (game side only)")
    w("")
    w("| feature | sites |")
    w("|---|---:|")
    for name, *_ in GX_PROBES:
        w(f"| {name} | {gx_hits.get(name, 0)} |")
    w("")

    def hist(title, counter):
        w(f"### {title}")
        w("")
        if not counter:
            w("_none_")
            w("")
            return
        w("| argument | sites |")
        w("|---|---:|")
        for k, v in counter.most_common():
            w(f"| `{k}` | {v} |")
        w("")

    hist("4.1 GXSetNumTevStages arguments", numtev_args)
    hist("4.2 GXSetNumChans arguments", numchan_args)
    hist("4.3 GXSetNumTexGens arguments", numtexgen_args)
    hist("4.4 GXSetNumIndStages arguments", numind_args)

    w("## 5. GX enum usage (game side only)")
    w("")
    for name, _ in ENUM_PROBES:
        c = enum_hits.get(name)
        if not c:
            continue
        w(f"### {name}")
        w("")
        w("| value | sites |")
        w("|---|---:|")
        for k, v in c.most_common():
            w(f"| `{k}` | {v} |")
        w("")

    # hazards
    w("## 6. Cross-build hazards (GCC 14 / powerpc-apple-darwin8)")
    w("")
    w("| hazard | sites | files |")
    w("|---|---:|---|")
    for name, _ in HAZARDS:
        hits = hazard_hits.get(name, [])
        total = sum(k for _, k in hits)
        files = ", ".join(f"`{f}`({k})" for f, k in sorted(hits, key=lambda x: -x[1])[:8])
        if len(hits) > 8:
            files += f", +{len(hits)-8} more"
        w(f"| {name} | {total} | {files or '--'} |")
    w("")
    w("Per-area breakdown of the two that matter most:")
    w("")
    for name in ("inline asm block  `asm {`", "paired-single intrinsic"):
        w(f"**{name}**")
        w("")
        for f, k in sorted(hazard_hits.get(name, []), key=lambda x: -x[1]):
            w(f"  * `{f}` -- {k}")
        w("")

    text = "\n".join(o) + "\n"
    if args.stdout:
        sys.stdout.write(text)
    else:
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(text)
        print(f"wrote {out_path} ({len(text):,} bytes, {len(seen_paths)} source files scanned)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
