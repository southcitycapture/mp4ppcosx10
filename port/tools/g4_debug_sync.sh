#!/bin/sh
# Ship the build tree's .o files (the DWARF debug map) to the G4 so gdb there
# can symbolise the running port, and install the mp4bt helper.
#
#   port/tools/g4_debug_sync.sh          after every port/build-ppc.sh
#
# gdb on the G4 (Xcode 3.0's gdb-768, installed 2026-09-16 from the Leopard
# disc's DeveloperToolsCLI.pkg) reads the debug map baked into the binary,
# which names /work/mp4/port/build-ppc-darwin/... -- the Docker mount path.
# On the G4 /work/mp4 is a root-made symlink to ~/mp4-work.  Objects are
# built -gdwarf-2 -gstrict-dwarf (Makefile DEBUG_G) because gdb-768 cannot
# read DWARF 5.  tar -p keeps mtimes so gdb does not warn that the .o is
# newer than the executable.
set -e
here=$(cd "$(dirname "$0")" && pwd)
port=$(cd "$here/.." && pwd)
G4=${G4_HOST:-g4}
ssh "$G4" 'mkdir -p ~/mp4-work/port ~/bin'
(cd "$port" && COPYFILE_DISABLE=1 tar --no-xattrs \
    --exclude build-ppc-darwin/MarioParty4.app \
    --exclude build-ppc-darwin/mp4peek.dSYM \
    -czf - build-ppc-darwin) | ssh "$G4" 'tar -xpzf - -C ~/mp4-work/port 2>&1 | grep -v "time stamp" || true'
scp -pq "$here/mp4bt" "$G4:~/bin/mp4bt"
ssh "$G4" 'chmod +x ~/bin/mp4bt; du -sh ~/mp4-work | cut -f1; ls -la /work/mp4 | cut -c1-60'
echo "g4_debug_sync: done -- on the G4: mp4bt [pid]  (backtrace of every thread; the game keeps running)"
