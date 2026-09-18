#!/bin/sh
# Install the Mario Party 4 bundle (and, once, the disc image) on the Power Mac G4.
#
#   port/tools/g4_install.sh [--image [DISC.iso]] [--no-app] [bundle.app]
#
# Why this exists rather than `g4 push`: the isle-ppc-tools helper's `push`
# writes the bundle straight into the runner slot `~/isle.app`, which on this G4
# is a *symlink* that `g4 use` flips between projects (Snowboard Kids 1, 2, LEGO
# Island, and now this).  Overwriting the slot with a real directory breaks
# `g4 use` for every project.  So the bundle is installed under its own name,
#
#     ~/MarioParty4.app
#
# exactly as the two Snowboard Kids bundles are, and the slot is pointed at it
# with `g4 use MarioParty4.app`.  Flipping back is `g4 use SnowboardKids2.app`.
#
# The disc image goes to ~/MarioParty4/ once (598 MB, several minutes on the
# wire); the port finds it there by itself.  --image re-sends it.
#
# Environment: G4_HOST (default `g4`, the ssh alias) is honoured, same as the
# g4 helper.
set -e
here=$(cd "$(dirname "$0")" && pwd)
: "${G4_HOST:=g4}"

send_app=1
send_image=0
image=
while [ $# -gt 0 ]; do
    case "$1" in
        --image)
            send_image=1
            shift
            case "$1" in
                ""|-*) ;;
                *.iso) image=$1; shift ;;
            esac
            ;;
        --no-app) send_app=0; shift ;;
        -*) echo "g4_install.sh: unknown option $1" >&2; exit 2 ;;
        *) break ;;
    esac
done

bundle=${1:-$here/../build-ppc-darwin/MarioParty4.app}
image=${image:-$here/../../orig/GMPE01_01/Mario Party 4 (USA) (Rev 1).nkit.iso}

if [ "$send_image" = 1 ]; then
    [ -f "$image" ] || { echo "no disc image at $image" >&2; exit 1; }
    echo "g4_install: sending $(du -h "$image" | cut -f1) of disc image to $G4_HOST:~/MarioParty4/"
    ssh "$G4_HOST" 'mkdir -p ~/MarioParty4'
    # One fixed name on the G4: the port globs for *.iso, and a name without
    # spaces keeps every remote sh command simple.
    scp "$image" "$G4_HOST:MarioParty4/mp4.nkit.iso"
fi

if [ "$send_app" = 1 ]; then
    [ -x "$bundle/Contents/MacOS/isle" ] || {
        echo "not a bundle with an isle executable: $bundle (run port/tools/make_bundle.sh)" >&2
        exit 1
    }
    base=$(basename "$bundle"); dir=$(dirname "$bundle")
    case "$base" in *" "*) echo "no spaces in the bundle name, please" >&2; exit 1;; esac
    echo "g4_install: pushing $base -> $G4_HOST:~/$base"
    # Tiger/Leopard ship GNU tar 1.14: plain ustar, no Apple metadata.  The
    # metadata flags are bsdtar's (the Mac); GNU tar on a Linux host
    # (littlejelly) has no --no-mac-metadata and no metadata to strip.
    tarflags="--format ustar"
    if [ "$(uname -s)" = Darwin ]; then
        tarflags="$tarflags --no-xattrs --no-acls --no-mac-metadata"
    fi
    COPYFILE_DISABLE=1 tar $tarflags \
        -C "$dir" -czf - "$base" \
    | ssh "$G4_HOST" "rm -rf .mp4-new && mkdir .mp4-new && tar -xzf - -C .mp4-new \
        && rm -rf '$base' && mv '.mp4-new/$base' '$base' && rmdir .mp4-new \
        && ls -la '$base/Contents/MacOS/isle'"
fi

ssh "$G4_HOST" 'echo "disc:  $(ls -la ~/MarioParty4/ 2>/dev/null | tail -n +2 | tail -2)"'
echo "g4_install: done.  Point the runner slot at it:   g4 use MarioParty4.app"
echo "            and back at Snowboard Kids after:      g4 use SnowboardKids2.app"
