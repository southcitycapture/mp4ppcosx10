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
# w06's fire blocks (PLAN.md 54b.5): the intro frame's draws explained and
# every texture it decodes written out, the render thread off (the probe's rule)
mkdir -p "$D/claw"; rm -f "$D/claw"/*
"$APP" --com4 --rtc dolphin --freshcard --noconfig --nomovies --play board-start-com4.play \
    --board 6 --ffto $((E + 150)) --frames $((E + 256)) --norenderthread --drawlog 4000 \
    --drawlog-at $((E + 250)) --dumptex --dumpframe $((E + 250)) --shotdir "$D/claw" > "$D/claw.log" 2>&1
echo "claw EXIT=$? $(date)" >> "$IDX"
echo "DONE" >> "$IDX"
[ "${LEAVE:-1}" = 1 ] && exec "$APP" --soak --com4 --rtc dolphin --freshcard --realtime --snap-every 5000 \
     --snap-keep 3 --status --ovllog --stuckwatch 200 --perf --board 1+
