#!/bin/sh
# M39 (PLAN.md 54.9): the named snapshots for the long soak's pauses, on the
# release candidate, as the console runner's job (`cp tools/m39_snaps.sh` into
# ~/MarioParty4-chain.app/Contents/MacOS/isle, `g4 use MarioParty4-chain.app;
# g4 run`).  The soak is deterministic under --rtc dolphin, so --ffto to just
# before a stall and --snap-at puts the state of that moment on disk; the run
# then carries on at real time with --dvdlog past the stall.  Then exec M38's
# leave-behind soak again.  Each run is killed by its own pid at its ceiling.
cd "$HOME"
APP="$HOME/MarioParty4.app/Contents/MacOS/isle"
D="$HOME/m39/final"; mkdir -p "$D"
SOAK="--soak --com4 --rtc dolphin --freshcard --status --perf --dvdlog"
run() {
    name=$1; ceiling=$2; shift 2
    t0=$(date +%s)
    "$APP" "$@" > "$D/$name.log" 2>&1 &
    pid=$!
    while kill -0 $pid 2>/dev/null; do
        sleep 5
        [ $(( $(date +%s) - t0 )) -gt "$ceiling" ] && { echo "snaps: $name over $ceiling s -- killed" >> "$D/$name.log"; kill -9 $pid; }
    done
    wait $pid; echo "$name EXIT=$? $(date)" >> "$D/index.txt"
}
# the Bowser space's cold read: data/bkoopa.bin at frame 65,362
mkdir -p "$D/snap-m39-bkoopa"
run snap-m39-bkoopa 3000 $SOAK --ffto 64800 --snap-at 65300 --snap-dir "$D/snap-m39-bkoopa" --frames 66000
# the first status-line stall: frame 229,735 (and 237,055 after it)
mkdir -p "$D/snap-m39-statusline"
run snap-m39-statusline 6000 $SOAK --ffto 229200 --snap-at 229700 --snap-dir "$D/snap-m39-statusline" --frames 237600
echo "snaps: leave-behind start $(date)" >> "$D/index.txt"
exec "$APP" --soak --com4 --rtc dolphin --freshcard --realtime --snap-every 5000 \
     --snap-keep 3 --status --ovllog --stuckwatch 200 --perf
