#!/bin/sh
# M39c chain 2: the w20 fault's reproduction on the faulting build (be6e6345,
# kept as ~/MarioParty4-be6e.app), --headless (no window: the G4's pboard had
# wedged and every windowed launch slept in SDL's clipboard check, PLAN.md
# 54b.3), a snapshot at 2000 and the restore of it to the fault; then
# m39c_chain.sh K A D P on the new build (chain 3 is that last line alone,
# after pboard was restarted)
cd "$HOME"
sleep 90
OLD="$HOME/MarioParty4-be6e.app/Contents/MacOS/isle"
D="$HOME/m39c/snap-m39c-w20-last5"; mkdir -p "$D"; rm -f "$D"/*.snap "$D"/run.log "$D"/restore.log
watchrun() { # log ceiling cmd...
    log=$1; ceil=$2; shift 2
    "$@" > "$log" 2>&1 &
    pid=$!; t0=$(date +%s)
    while kill -0 $pid 2>/dev/null; do sleep 5
        [ $(( $(date +%s) - t0 )) -gt $ceil ] && { echo "chain2: over $ceil s -- killed" >> "$log"; kill -9 $pid; }
        grep -q "port: fatal" "$log" && { sleep 4; kill -9 $pid 2>/dev/null; }
    done
    wait $pid; RC=$?
}
watchrun "$D/run.log" 900 "$OLD" --headless --goto w20dll:0:0 --com4 --rtc dolphin --freshcard --noconfig \
    --turns 3 --status --perf --ovllog --snap-at 2000 --snap-dir "$D" --frames 6000
echo "repro EXIT=$RC $(date)" >> "$D/index.txt"
S=$(ls "$D"/*.snap 2>/dev/null | head -1)
if [ -n "$S" ]; then
    watchrun "$D/restore.log" 900 "$OLD" --headless --restore "$S" --com4 --rtc dolphin --freshcard --noconfig \
        --turns 3 --status --perf --ovllog --frames 6000
    echo "restore EXIT=$RC $(date)" >> "$D/index.txt"
fi
echo "DONE" >> "$D/index.txt"
PHASES="K A D P" M39C_SETTLE=5 exec sh "$HOME/m39c_chain.sh"
