#!/bin/sh
# M32: the disk image a player gets -- "Mario Party 4 PowerPC Edition 0.9.dmg".
#
#   port/tools/make_dmg.sh [bundle.app] [out.dmg]
#
# The image holds one folder, in the shape the Snowboard Kids 1+2 PowerPC
# Edition shipped in (the user's other G4 ports):
#
#     Mario Party 4 PowerPC Edition/
#         Mario Party 4.app        the bundle make_bundle.sh built, renamed
#         Read Me.txt              port/dist/Read Me.txt
#         Licences/                MusyX (MIT), SDL2 (zlib), the port's own note
#
# NO GAME DATA, EVER: the image is built from the bundle alone, and the
# script refuses a bundle with a disc image in its Resources (--with-image).
#
# hdiutil is a Mac tool.  On a Mac (the G4, the MacBook) this script runs it
# directly; on littlejelly (Linux) it stages the folder, streams it to the
# G4 over ssh with GNU tar, runs hdiutil there (UDZO, the compressed
# read-only format the Snowboard Kids image uses), pulls the .dmg back and
# leaves the G4's copy at ~/<name>.dmg for the fresh-install test.  A few
# megabytes and a few seconds on the G4's disk: run it while no soak runs.
#
# Environment: G4_HOST (default g4).  The version comes from include/port.h
# (PORT_VERSION_STRING), the same place make_bundle.sh reads it.
set -e
here=$(cd "$(dirname "$0")" && pwd)
: "${G4_HOST:=g4}"

bundle=${1:-$here/../build-ppc-darwin/MarioParty4.app}
version=$(sed -n 's/^#define PORT_VERSION_STRING "\(.*\)"/\1/p' "$here/../include/port.h")
version=${version:-0.9}
name="Mario Party 4 PowerPC Edition"
out=${2:-$here/../build-ppc-darwin/$name $version.dmg}

[ -x "$bundle/Contents/MacOS/isle" ] || {
    echo "not a bundle with an isle executable: $bundle (run port/tools/make_bundle.sh)" >&2
    exit 1
}
if ls "$bundle/Contents/Resources"/*.iso "$bundle/Contents/Resources"/files >/dev/null 2>&1; then
    echo "make_dmg: the bundle carries a disc image (Contents/Resources); the disk image" >&2
    echo "          is built from a bundle with no game data in it.  Refusing." >&2
    exit 1
fi
[ -f "$here/../dist/Read Me.txt" ] || { echo "no port/dist/Read Me.txt" >&2; exit 1; }

stage=$(mktemp -d "${TMPDIR:-/tmp}/mp4dmg.XXXXXX")
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/$name"
cp -R "$bundle" "$stage/$name/Mario Party 4.app"
# the lab's input scripts are not a player's business, but they are small and
# the harness flags that use them are documented; they stay
cp "$here/../dist/Read Me.txt" "$stage/$name/Read Me.txt"
mkdir -p "$stage/$name/Licences"
cp "$here/../dist/Licences/"*.txt "$stage/$name/Licences/"
# a last look: nothing in the staged tree may be a disc image
if find "$stage" -size +100M | grep -q .; then
    echo "make_dmg: a file over 100 MB is in the staged folder; that is not the app.  Refusing." >&2
    find "$stage" -size +100M >&2
    exit 1
fi
echo "make_dmg: staged $(du -sh "$stage/$name" | cut -f1) in $stage/$name"

if [ "$(uname -s)" = Darwin ]; then
    rm -f "$out"
    hdiutil create -quiet -srcfolder "$stage/$name" -volname "$name" -format UDZO \
        -imagekey zlib-level=9 "$out"
else
    # littlejelly: build it on the G4.  GNU tar 1.14 there: plain ustar.
    remote_dmg="$name $version.dmg"
    echo "make_dmg: building on $G4_HOST with hdiutil"
    tar --format ustar -C "$stage" -cf - "$name" \
    | ssh "$G4_HOST" "rm -rf .mp4-dmg && mkdir .mp4-dmg && tar -xf - -C .mp4-dmg \
        && rm -f '$remote_dmg' \
        && hdiutil create -quiet -srcfolder '.mp4-dmg/$name' -volname '$name' -format UDZO \
               -imagekey zlib-level=9 '$remote_dmg' \
        && rm -rf .mp4-dmg && ls -la '$remote_dmg' && md5 '$remote_dmg'"
    rm -f "$out"
    ssh "$G4_HOST" "cat '$remote_dmg'" > "$out"   # scp mangles a remote path with spaces
    echo "make_dmg: the G4 keeps a copy at ~/$remote_dmg"
fi
echo "dmg: $out"
ls -la "$out"
md5sum "$out" 2>/dev/null || md5 "$out"
