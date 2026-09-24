#!/bin/sh
# M43 (PLAN.md 58.3): the PGO build.  The training runs (tools/m43_chain.sh
# P:GAME, the -fprofile-generate bundle ~/mp4-pgogen.app) leave their counters
# under the G4's ~/pgo/work/mp4/port/build-m43-pgogen (GCOV_PREFIX); this
# brings them back beside the objects of a -fprofile-use tree and builds it.
#   tools/m43_pgo.sh [USE_TREE]        (default build-m43-pgouse)
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
use=${1:-build-m43-pgouse}
cd "$here"
rm -rf /tmp/m43-pgo && mkdir -p /tmp/m43-pgo
ssh g4 'cd ~/pgo/work/mp4/port/build-m43-pgogen && tar cf - $(find . -name "*.gcda")' | tar xf - -C /tmp/m43-pgo
echo "m43_pgo: $(find /tmp/m43-pgo -name '*.gcda' | wc -l) .gcda files from the G4"
mkdir -p "$use"
(cd /tmp/m43-pgo && find . -name '*.gcda') | while read f; do
    mkdir -p "$use/$(dirname "$f")"
    cp "/tmp/m43-pgo/$f" "$use/$f"
done
./build-ppc.sh -j4 BUILD="$use" PGO=use
