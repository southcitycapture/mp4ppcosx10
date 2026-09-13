#!/bin/sh
# Cross-build the Mario Party 4 port for the Power Mac G4 (Mac OS X 10.4/10.5)
# inside the gcc-powerpc-apple-darwin8 Docker image -- GCC 14.2 with the
# MacOSX10.4u SDK, the same toolchain the two Snowboard Kids ports use.
#
#   port/build-ppc.sh [make targets/vars...]     e.g.  port/build-ppc.sh -j8
#
# Mounts:
#   /work/mp4   -> this repository, read-write (output lands in port/build-ppc-darwin)
#   /work/sdl2  -> the Tiger SDL2 prefix, read-only, if one is present
#
# M1 needs no SDL2 at all: there is no window and no audio device yet, only the
# game's own boot narration on stdout.  The mount is still made when the prefix
# exists so M2 can turn the window on without touching this script.
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
SDL2_PREFIX=${SDL2_PREFIX:-$HOME/Apps/panther-sdl2/build-tiger-joy/prefix}
IMAGE=${IMAGE:-ghcr.io/variantxyz/gcc-powerpc-apple-darwin8:build-gcc-14.2-MacOSXSDK10.4u}

mkdir -p "$here/build-ppc-darwin"

sdl_mount=""
if [ -d "$SDL2_PREFIX/include/SDL2" ]; then
    sdl_mount="-v $SDL2_PREFIX:/work/sdl2:ro"
fi

exec docker run --rm \
    -v "$repo":/work/mp4 \
    $sdl_mount \
    -w /work/mp4/port \
    "$IMAGE" \
    make -f Makefile TARGET=ppc-darwin SDL2_PREFIX=/work/sdl2 "$@" \
    2>&1 | grep -v "requested image's platform"
