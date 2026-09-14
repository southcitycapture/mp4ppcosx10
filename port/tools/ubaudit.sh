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
# Usage:  port/tools/ubaudit.sh            (needs the mirror: build once first)
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
