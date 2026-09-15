#!/bin/sh
# Cross-build port/tools/mp4peek.c for the G4 (see the file's own header).
# It is a debugging tool, not part of the port, so it has its own one-line
# build rather than a Makefile target.
#
#   port/tools/build-peek.sh          -> port/build-ppc-darwin/mp4peek
#
# It compiles against the *mirrored* headers (port/build-ppc-darwin/gen/include),
# i.e. the exact declarations the running binary was built from, with the same
# -malign-natural and the same GAME_DEFS -- so offsetof() here is offsetof()
# there.  Run `port/build-ppc.sh` at least once first so the mirror exists.
# The decomp's include/ shadows <stdint.h> and friends, hence -idirafter.
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
IMAGE=${IMAGE:-ghcr.io/variantxyz/gcc-powerpc-apple-darwin8:build-gcc-14.2-MacOSXSDK10.4u}

mkdir -p "$repo/port/build-ppc-darwin"
exec docker run --rm -v "$repo":/work/mp4 -w /work/mp4 "$IMAGE" \
    /usr/local/bin/powerpc-apple-darwin8-gcc \
        -isysroot /usr/local/MacOSX10.4u.sdk -mmacosx-version-min=10.4 \
        -malign-natural -mone-byte-bool -O1 -g -w -std=gnu11 -fno-strict-aliasing \
        -DTARGET_PC -DVERSION=1 -DNDEBUG=1 -DMTX_USE_C -DMUSY_TARGET=MUSY_TARGET_PC \
        -DPORT_PRC_STACK_MUL=4 -DPORT_PRC_STACK_GUARD=64 \
        -idirafter port/build-ppc-darwin/gen/include \
        -idirafter extern/musyx/include \
        -idirafter build/GMPE01_01/include \
        -idirafter port/include \
        -o port/build-ppc-darwin/mp4peek port/tools/mp4peek.c \
    2>&1 | grep -v "requested image's platform" || true
