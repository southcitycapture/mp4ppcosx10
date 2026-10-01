#!/bin/sh
# M39c (PLAN.md 54b.6): the fps-boards.md measurements, as the console
# runner's job (`cp tools/m39c_perf.sh` into
# ~/MarioParty4-chain.app/Contents/MacOS/isle; `g4 run`).  Per board 1-6 the
# same walk as the survey (--nomovies: the board is entered at frame E =
# 5108), fast-forwarded to E+2700 (no drawing), then at real time to E+3900:
# the host's first Star and the first turns.  --perfdump writes every
# retrace's costs (the table reads E+3000..E+3900, the cache warm), --gltrace
# arms the forty drawn frames up to E+3800 (every GL call), and the exit
# report's GX draw totals over the drawn window give the vertices.  Then exec
# the leave-behind soak (LEAVE=0 to skip it).
cd "$HOME"
sleep "${SETTLE:-10}"
APP="$HOME/MarioParty4.app/Contents/MacOS/isle"
D="$HOME/m39c/perf"; mkdir -p "$D"
IDX="$D/index.txt"
E=${BOARD_ENTRY:-5108}
echo "# m39c perf start $(date)  isle md5 $(md5 -q "$APP")" >> "$IDX"
for n in ${BOARDS:-1 2 3 4 5 6}; do
    log="$D/P$n.log"
    "$APP" --com4 --rtc dolphin --freshcard --noconfig --nomovies --play board-start-com4.play \
        --board $n --ffto $((E + 2700)) --frames $((E + 3900)) --perf --perfdump "$D/P$n.csv" \
        --gltrace $((E + 3800)) --status > "$log" 2>&1 &
    pid=$!; t0=$(date +%s)
    while kill -0 $pid 2>/dev/null; do
        sleep 5
        [ $(( $(date +%s) - t0 )) -gt 900 ] && { echo "perf: P$n over 900 s -- killed" >> "$log"; kill -9 $pid; }
    done
    wait $pid; echo "P$n EXIT=$? $(date)" >> "$IDX"
    sleep 5
done
# w06's fire blocks (PLAN.md 54b.5): which object is the claw -- the intro
# frame with each candidate object of model 18 left out (the drawlog's
# 64x128 CMPR draws: "0" "3" "5" "7" "10" and "cube2"), and the claw's
# named snapshot (the board's intro, frame 5300)
for o in 0 3 5 7 10 cube2; do
    mkdir -p "$D/claw-skip-$o"; rm -f "$D/claw-skip-$o"/*
    "$APP" --com4 --rtc dolphin --freshcard --noconfig --nomovies --play board-start-com4.play \
        --board 6 --ffto $((E + 150)) --frames $((E + 256)) --skipobj "$o" --dumpframe $((E + 250)) \
        --shotdir "$D/claw-skip-$o" > "$D/claw-skip-$o.log" 2>&1
    echo "claw-skip-$o EXIT=$? $(date)" >> "$IDX"
done
mkdir -p "$D/snap-m39c-w06-claw"
"$APP" --com4 --rtc dolphin --freshcard --noconfig --nomovies --play board-start-com4.play \
    --board 6 --ffto $((E + 100)) --snap-at $((E + 192)) --snap-dir "$D/snap-m39c-w06-claw" \
    --frames $((E + 256)) --dumpframe $((E + 250)) --shotdir "$D/snap-m39c-w06-claw" \
    > "$D/snap-m39c-w06-claw.log" 2>&1
echo "snap-m39c-w06-claw EXIT=$? $(date)" >> "$IDX"
# the named snapshots of the realtime runs' resyncs (PLAN.md 54b.2): the
# same flags as the run (movies on), fast-forwarded to just before, a
# snapshot on disk, then the moment again at real time with --dvdlog
snap() {
    name=$1; ff=$2; at=$3; end=$4; shift 4
    mkdir -p "$D/$name"
    "$APP" --soak --com4 --rtc dolphin --freshcard --status --perf --dvdlog "$@" \
        --ffto $ff --snap-at $at --snap-dir "$D/$name" --frames $end > "$D/$name.log" 2>&1 &
    pid=$!; t0=$(date +%s)
    while kill -0 $pid 2>/dev/null; do
        sleep 5
        [ $(( $(date +%s) - t0 )) -gt 1800 ] && { echo "perf: $name over 1800 s -- killed" >> "$D/$name.log"; kill -9 $pid; }
    done
    wait $pid; echo "$name EXIT=$? $(date)" >> "$IDX"
}
# w04: 1.1 s behind at the first instruction card (R4, retrace 13440)
snap snap-m39c-w04-inst 12900 13300 14200 --board 4 --turns 5
# w02: the board's end, the card image's rename 1.75 s (R2, frame 60197)
snap snap-m39c-w02-end 59600 60100 61000 --board 2 --turns 5
echo "DONE" >> "$IDX"
[ "${LEAVE:-1}" = 1 ] && exec "$APP" --soak --com4 --rtc dolphin --freshcard --realtime --snap-every 5000 \
     --snap-keep 3 --status --ovllog --stuckwatch 200 --perf --board 1+
