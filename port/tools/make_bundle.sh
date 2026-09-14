#!/bin/sh
# Wrap the cross-built PowerPC executable into a Mac OS X .app bundle for the G4.
#
#   port/tools/make_bundle.sh [--with-image [DISC.iso]] [exe] ["out.app"]
#
# The executable inside is named `isle`, not `marioparty4`, for one reason: the
# isle-ppc-tools console runner on the G4 hard-codes
# `~/isle.app/Contents/MacOS/isle`, and `~/isle.app` is a symlink that
# `g4 use NAME.app` flips between projects.  Naming it `isle` is what lets
# `g4 use MarioParty4.app` / `g4 run` / `g4 shot` / `g4 push-bin` all work
# unchanged, the same trick the two Snowboard Kids bundles use.
#
# No disc image goes in by default.  598 MB inside the bundle means every
# `g4 push` re-sends it over the wire, so the image normally lives in the
# shared folder
#
#     ~/MarioParty4/
#
# which the port searches at startup (see port_find_default_image in
# src/platform/main.c) and which survives replacing the .app.  The repository
# has never contained a disc image and neither does what it builds.
#
# --with-image is for a genuinely self-contained bundle: it copies the image
# into Contents/Resources, which the port searches first.
set -e
here=$(cd "$(dirname "$0")" && pwd)

with_image=0
image=
while [ $# -gt 0 ]; do
    case "$1" in
        --with-image)
            with_image=1
            shift
            case "$1" in
                ""|-*) ;;
                *.iso) image=$1; shift ;;
            esac
            ;;
        --no-image) with_image=0; shift ;;
        --) shift; break ;;
        -*) echo "make_bundle.sh: unknown option $1" >&2; exit 2 ;;
        *) break ;;
    esac
done

exe=${1:-$here/../build-ppc-darwin/marioparty4}
out=${2:-$here/../build-ppc-darwin/MarioParty4.app}
image=${image:-$here/../../orig/GMPE01_01/Mario Party 4 (USA) (Rev 1).nkit.iso}

[ -f "$exe" ] || { echo "no executable at $exe (run port/build-ppc.sh)" >&2; exit 1; }
if [ "$with_image" = 1 ] && [ ! -f "$image" ]; then
    echo "--with-image but no disc image at $image" >&2
    exit 1
fi

rm -rf "$out"
mkdir -p "$out/Contents/MacOS" "$out/Contents/Resources"
cp "$exe" "$out/Contents/MacOS/isle"
chmod 755 "$out/Contents/MacOS/isle"
# The 99 REL modules, one dlopen'ed Mach-O bundle each.  They go next to the
# executable because that is where --reldir defaults to (<exe dir>/rels), so
# nothing has to be told where they are.
rels=$(dirname "$exe")/rels
if [ -d "$rels" ]; then
    mkdir -p "$out/Contents/MacOS/rels"
    cp "$rels"/*.bundle "$out/Contents/MacOS/rels/" 2>/dev/null || cp -R "$rels"/. "$out/Contents/MacOS/rels/"
    echo "  rels: $(ls "$out/Contents/MacOS/rels" | wc -l | tr -d ' ') modules"
fi
if [ "$with_image" = 1 ]; then
    cp "$image" "$out/Contents/Resources/$(basename "$image")"
fi
# The input scripts.  `--play board-start.play` is how every runbook writes it,
# and the runner's working directory on the G4 is not this tree; pad_play.c
# falls back to Contents/Resources/movies for a name with no '/' in it, so the
# scripts have to be in the bundle for that to find anything.  They are a few
# kilobytes each.
movies=$here/../ref/movies
if [ -d "$movies" ]; then
    mkdir -p "$out/Contents/Resources/movies"
    cp "$movies"/*.play "$out/Contents/Resources/movies/" 2>/dev/null || true
    echo "  movies: $(ls "$out/Contents/Resources/movies" | wc -l | tr -d ' ') input scripts"
fi
# SDL2.  The cross build links it from the Docker mount (/work/sdl2/lib), a path
# that does not exist on the G4, so the dylib is copied into the bundle and the
# executable's reference to it rewritten to @executable_path.  Nothing has to be
# installed on the G4 and DYLD_LIBRARY_PATH stays out of it.  (The Snowboard
# Kids ports link SDL2 statically and need none of this; if this port ever does
# the same, the whole block becomes a no-op.)
SDL2_PREFIX=${SDL2_PREFIX:-$HOME/Apps/panther-sdl2/build-tiger-joy/prefix}
IMAGE=${IMAGE:-ghcr.io/variantxyz/gcc-powerpc-apple-darwin8:build-gcc-14.2-MacOSXSDK10.4u}
sdl_ref=$(otool -L "$out/Contents/MacOS/isle" 2>/dev/null | awk '/libSDL2/{print $1}' | head -1)
if [ -n "$sdl_ref" ]; then
    sdl_leaf=$(basename "$sdl_ref")
    if [ -f "$SDL2_PREFIX/lib/$sdl_leaf" ]; then
        mkdir -p "$out/Contents/Frameworks"
        cp "$SDL2_PREFIX/lib/$sdl_leaf" "$out/Contents/Frameworks/$sdl_leaf"
        chmod 755 "$out/Contents/Frameworks/$sdl_leaf"
        # The host's own install_name_tool refuses these 2005-vintage PowerPC
        # Mach-Os ("malformed load command 0"), so use the cross toolchain's,
        # which is already in the build image.
        docker run --rm -v "$(cd "$out/.." && pwd)":/w -w /w "$IMAGE" sh -c "
            powerpc-apple-darwin8-install_name_tool -change '$sdl_ref' \
                '@executable_path/../Frameworks/$sdl_leaf' \
                '$(basename "$out")/Contents/MacOS/isle' &&
            powerpc-apple-darwin8-install_name_tool -id \
                '@executable_path/../Frameworks/$sdl_leaf' \
                '$(basename "$out")/Contents/Frameworks/$sdl_leaf'" \
            2>&1 | grep -v "requested image's platform" || true
        echo "  sdl2: $sdl_leaf -> Contents/Frameworks (was $sdl_ref)"
    else
        echo "  WARNING: isle needs $sdl_ref and there is no $sdl_leaf in $SDL2_PREFIX/lib;" >&2
        echo "           the bundle will not start on a machine without that path." >&2
    fi
fi
[ -f "$here/../resources/MarioParty4.icns" ] && \
    cp "$here/../resources/MarioParty4.icns" "$out/Contents/Resources/MarioParty4.icns"
cat > "$out/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key><string>English</string>
	<key>CFBundleExecutable</key><string>isle</string>
	<key>CFBundleIconFile</key><string>MarioParty4</string>
	<key>CFBundleIdentifier</key><string>com.southcitycapture.marioparty4</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleName</key><string>Mario Party 4</string>
	<key>CFBundleDisplayName</key><string>Mario Party 4</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleSignature</key><string>????</string>
	<key>CFBundleShortVersionString</key><string>1.0</string>
	<key>CFBundleVersion</key><string>1.0</string>
	<key>CFBundleGetInfoString</key><string>Mario Party 4 PowerPC Edition</string>
	<key>LSMinimumSystemVersion</key><string>10.4</string>
	<key>LSApplicationCategoryType</key><string>public.app-category.games</string>
	<key>NSHighResolutionCapable</key><false/>
</dict>
</plist>
PLIST
printf 'APPL????' > "$out/Contents/PkgInfo"
echo "bundle: $out"
[ "$with_image" = 1 ] || echo "  (no disc image inside: put yours in ~/MarioParty4/ on the G4)"
ls -la "$out/Contents/MacOS" "$out/Contents/Resources"
