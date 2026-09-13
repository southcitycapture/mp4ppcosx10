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
if [ "$with_image" = 1 ]; then
    cp "$image" "$out/Contents/Resources/$(basename "$image")"
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
