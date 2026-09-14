#!/bin/sh
# ubaudit.sh -- find the loops GCC is entitled to run forever.
#
# GCC's -faggressive-loop-optimizations reads an out-of-bounds array access as
# proof that the iteration performing it never happens.  It then uses that to
# bound the loop -- and, if the loop's own exit test is now redundant under
# that bound, to DELETE the exit test.  In ordinary C that is sound.  In a
# decompilation it is not, because every struct array length in the tree is a
# reconstruction, and a length that is one element short of what the code
# really indexes turns the proof into a licence to remove the only thing
# keeping a pointer inside memory.
#
# That is exactly the m425dll crash (PLAN.md 18.2): `s32 unk_3C[5]` against a
# `var_r29 < 6` loop cost fn_1_E914 its bound and let it write -1/0/0.0f every
# four bytes for 16 MB past the top of MEM1.
#
# The build compiles the game with -w, so none of this is visible.  This
# script recompiles the mirrored tree with the two warnings that name it:
#
#   -Waggressive-loop-optimizations   "iteration N invokes undefined behavior"
#                                     -- GCC used UB to bound this loop.  Check
#                                     whether the loop's bound exceeds the
#                                     array it indexes; if it does, the exit
#                                     test is gone.
#   -Warray-bounds                    the constant-index form of the same bug.
#
# Usage:  port/tools/ubaudit.sh            list the sites
#         port/tools/ubaudit.sh --triage   say which ones GCC actually acted on
#
# --triage is a shortlist, and it is worth being exact about what it is not.
#
# The warning fires whenever GCC used an out-of-bounds access to bound a loop,
# and that is harmless when the loop's own bound is already tight -- `for (j =
# 0; j < 4; j++)` over a [4] array warns about the iteration that never
# happens.  It is a bug only when the loop's bound *exceeds* the array, because
# then the test GCC removed was doing real work.
#
# So --triage compiles each **warning's** file twice, with and without
# -fno-aggressive-loop-optimizations, and prints every function whose
# instruction count differs.  A function that gains four or five instructions
# with the optimisation off has probably had a compare and a branch put back,
# which is the m425dll shape (fn_1_E914 gained 319).
#
# What it is not: a codegen difference is NOT specific to a deleted bound.
# -faggressive-loop-optimizations changes loop analysis generally, and over the
# whole tree it perturbs a great many functions in files that emit no warning
# at all -- eighteen files in the first third alone, of which two warn.  That
# is why this pass runs the warning scan first and only triages the files it
# named.  Even then the answer is "read these eight functions", not "these
# eight are bugs": the verdict is in the disassembly, where you look for a
# compare against the loop's constant bound and find it missing.
#
# Of the 39 sites in 20 modules at M8, --triage narrowed the reading to 8
# functions in 6 files.  It takes about a minute.
#
# (needs the mirror: build once first)
#
# The fix for any one site is a patches.txt entry correcting the declaration.
# -fno-aggressive-loop-optimizations in GAME_CFLAGS is what holds the line for
# the ones not yet corrected.
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
IMAGE=${IMAGE:-ghcr.io/variantxyz/gcc-powerpc-apple-darwin8:build-gcc-14.2-MacOSXSDK10.4u}

if [ ! -d "$repo/port/build-ppc-darwin/gen/src" ]; then
    echo "ubaudit: no mirror at port/build-ppc-darwin/gen -- run port/build-ppc.sh first" >&2
    exit 1
fi

MODE=list
if [ "$1" = "--triage" ]; then
    MODE=triage
fi
if [ "$1" = "--prove" ]; then
    MODE=prove
    shift
    if [ -z "$1" ]; then
        echo "ubaudit --prove: give a path under the mirror, e.g. src/REL/m453Dll/score.c" >&2
        exit 2
    fi
fi

# --prove FILE: the per-file form of the m425 proof (PLAN.md 18.2, 18.10).
#
# It compiles ONE mirrored file twice, with and without
# -fno-aggressive-loop-optimizations, prints the warnings and the per-function
# instruction counts, and says for each function whether the two agree.
#
# The mirror already has patches.txt applied, so the workflow for a correction
# is: --prove the file (that is the UNFIXED pair), add the patch, re-run
# tools/mirror_src.py, --prove again (the FIXED pair).  Two things then have to
# hold, and they say different things:
#
#   fixed-on == fixed-off        no undefined access is left for the
#                                optimisation to act on -- the safety-net flag
#                                is now a no-op for this file
#   fixed-on == unfixed-off      the correction restored exactly the code the
#                                flag was restoring, i.e. the declaration was
#                                the whole of the bug and the patch changed
#                                nothing else
#
# The second equality only holds for a patch that is purely a declaration
# merge.  A patch that also corrects a loop bound in the game's own code
# changes the program, so it keeps the first equality and loses the second --
# which is the honest signal that a behaviour change was made, and it should be
# argued for in patches.txt rather than hidden.
if [ "$MODE" = "prove" ]; then
exec docker run --rm -v "$repo":/work/mp4 -w /work/mp4/port "$IMAGE" sh -c '
cd /work/mp4/port
f="build-ppc-darwin/gen/'"$1"'"
if [ ! -f "$f" ]; then echo "ubaudit --prove: no such mirrored file: $f" >&2; exit 1; fi
CF="-std=gnu11 -O2 -fno-strict-aliasing -fwrapv -fcommon -w
 -Wno-implicit-function-declaration -Wno-return-mismatch -Wno-int-conversion
 -Wno-incompatible-pointer-types -Wno-incompatible-function-pointer-types
 -Wno-unknown-pragmas -Wno-implicit-int -Wno-builtin-declaration-mismatch
 -Wno-discarded-qualifiers -Wno-pointer-sign
 -DTARGET_PC -DVERSION=1 -DNDEBUG=1 -DMTX_USE_C -DMUSY_TARGET=MUSY_TARGET_PC
 -DPORT_PRC_STACK_MUL=4 -DPORT_PRC_STACK_GUARD=64
 -Ibuild-ppc-darwin/gen/include -I../extern/musyx/include
 -I../build/GMPE01_01/include -Iinclude
 -isysroot /usr/local/MacOSX10.4u.sdk -mmacosx-version-min=10.4
 -malign-natural -mone-byte-bool"
WF=$(echo $CF | sed "s/-w //")
counts() {
  llvm-objdump -d --triple=powerpc-apple-darwin "$1" 2>/dev/null \
  | awk "/^[0-9a-f]+ <_/{name=\$2; next} /^ +[0-9a-f]+:/{c[name]++} END{for(n in c) print n, c[n]}" \
  | sort
}
echo "== $f"
echo "-- warnings"
powerpc-apple-darwin8-gcc $WF -Waggressive-loop-optimizations -Warray-bounds \
  -c -o /tmp/ub_w.o "$f" 2>&1 \
  | grep -E "aggressive-loop-optimizations|array-bounds" \
  | sed "s|build-ppc-darwin/gen/||" || true
powerpc-apple-darwin8-gcc $CF -c -o /tmp/ub_on.o "$f"
powerpc-apple-darwin8-gcc $CF -fno-aggressive-loop-optimizations -c -o /tmp/ub_off.o "$f"
counts /tmp/ub_on.o  > /tmp/ub_on.txt
counts /tmp/ub_off.o > /tmp/ub_off.txt
echo "-- per-function instruction counts (aggressive on / off)"
join /tmp/ub_on.txt /tmp/ub_off.txt \
  | awk "{ printf \"   %-28s %6s %6s   %s\\n\", \$1, \$2, \$3, (\$2==\$3 ? \"same\" : \"DIFFER\") }"
n=$(join /tmp/ub_on.txt /tmp/ub_off.txt | awk "\$2 != \$3" | wc -l | tr -d " ")
echo "-- $n function(s) differ"
' 2>&1 | grep -v "requested image.s platform"
fi

if [ "$MODE" = "triage" ]; then
exec docker run --rm -v "$repo":/work/mp4 -w /work/mp4/port "$IMAGE" sh -c '
cd /work/mp4/port
CF="-std=gnu11 -O2 -fno-strict-aliasing -fwrapv -fcommon -w
 -Wno-implicit-function-declaration -Wno-return-mismatch -Wno-int-conversion
 -Wno-incompatible-pointer-types -Wno-incompatible-function-pointer-types
 -Wno-unknown-pragmas -Wno-implicit-int -Wno-builtin-declaration-mismatch
 -Wno-discarded-qualifiers -Wno-pointer-sign
 -DTARGET_PC -DVERSION=1 -DNDEBUG=1 -DMTX_USE_C -DMUSY_TARGET=MUSY_TARGET_PC
 -DPORT_PRC_STACK_MUL=4 -DPORT_PRC_STACK_GUARD=64
 -Ibuild-ppc-darwin/gen/include -I../extern/musyx/include
 -I../build/GMPE01_01/include -Iinclude
 -isysroot /usr/local/MacOSX10.4u.sdk -mmacosx-version-min=10.4
 -malign-natural -mone-byte-bool"
# The same flags without -w, because -w suppresses the warning we probe for.
WF=$(echo $CF | sed "s/-w //")
counts() {
  llvm-objdump -d --triple=powerpc-apple-darwin "$1" 2>/dev/null \
  | awk "/^[0-9a-f]+ <_/{name=\$2; next} /^ +[0-9a-f]+:/{c[name]++} END{for(n in c) print n, c[n]}" \
  | sort
}
# Only the files that actually warn: a codegen difference on its own is not
# evidence of anything (see the note at the top).
find build-ppc-darwin/gen/src -name "*.c" | sort | while read f; do
  powerpc-apple-darwin8-gcc $WF -Waggressive-loop-optimizations -c -o /tmp/ub_w.o "$f" 2>&1 \
    | grep -q "aggressive-loop-optimizations" || continue
  powerpc-apple-darwin8-gcc $CF -c -o /tmp/ub_on.o "$f" 2>/dev/null || continue
  powerpc-apple-darwin8-gcc $CF -fno-aggressive-loop-optimizations -c -o /tmp/ub_off.o "$f" 2>/dev/null || continue
  counts /tmp/ub_on.o  > /tmp/ub_on.txt
  counts /tmp/ub_off.o > /tmp/ub_off.txt
  d=$(join /tmp/ub_on.txt /tmp/ub_off.txt | awk "\$2 != \$3 {printf \"      %s  %s -> %s (aggressive on -> off)\\n\", \$1, \$2, \$3}")
  if [ -n "$d" ]; then
    echo "== $(echo $f | sed "s|build-ppc-darwin/gen/src/||")"
    echo "$d"
  fi
done
' 2>&1 | grep -v "requested image.s platform"
fi

exec docker run --rm -v "$repo":/work/mp4 -w /work/mp4/port "$IMAGE" sh -c '
cd /work/mp4/port
n=0
find build-ppc-darwin/gen/src -name "*.c" | sort | while read f; do
  n=$((n+1))
  powerpc-apple-darwin8-gcc -std=gnu11 -O2 -fno-strict-aliasing -fwrapv -fcommon \
    -Wno-implicit-function-declaration -Wno-return-mismatch \
    -Wno-int-conversion -Wno-incompatible-pointer-types \
    -Wno-incompatible-function-pointer-types -Wno-unknown-pragmas -Wno-implicit-int \
    -Wno-builtin-declaration-mismatch -Wno-discarded-qualifiers -Wno-pointer-sign \
    -Waggressive-loop-optimizations -Warray-bounds \
    -DTARGET_PC -DVERSION=1 -DNDEBUG=1 -DMTX_USE_C -DMUSY_TARGET=MUSY_TARGET_PC \
    -DPORT_PRC_STACK_MUL=4 -DPORT_PRC_STACK_GUARD=64 \
    -I build-ppc-darwin/gen/include -I ../extern/musyx/include \
    -I ../build/GMPE01_01/include -Iinclude \
    -isysroot /usr/local/MacOSX10.4u.sdk -mmacosx-version-min=10.4 \
    -malign-natural -mone-byte-bool \
    -c -o /tmp/ubaudit.o "$f" 2>&1 \
  | grep -E "aggressive-loop-optimizations|array-bounds=" \
  | sed "s|build-ppc-darwin/gen/src/||"
done
' 2>&1 | grep -v "requested image's platform"
