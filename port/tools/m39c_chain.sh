#!/bin/sh
# M39c (PLAN.md 54b): the other boards, as the console runner's job
# (`cp tools/m39c_chain.sh` into ~/MarioParty4-chain.app/Contents/MacOS/isle,
# `g4 use MarioParty4-chain.app; g4 run`).  Phases (PHASES="A B C D L" by
# default, each a letter):
#   A  the lever's witness: the md5 walks M (movies) / N (--nomovies), the
#      same with --board 1 (MB1 / NB1: must be byte-identical), and NB2
#      (--board 2: frame 7000 must be Goomba's board)
#   B  the board survey: boards 1-6 at lockstep with --nomovies, the menu's
#      own 20 turns (as the console's run: a short --turns puts the last-five-
#      turns event on turn 1), turn 1 and into turn 2, a frame dumped every
#      250 from the board's entry
#      (--boarddump); the gallery's port frames are picked from these
#   C  every other board played: w02-w06 at real time, the player defaults
#      plus --soak --com4 --rtc dolphin --freshcard --status --perf --board N
#      --turns 5 (+ --ovllog --dvdlog: logging only, the events' evidence),
#      SIGINT when the soak is back in modeseldll after the board
#   D  the extras by --goto: w10dll (the tutorial), w20dll/w21dll (the Extra
#      Room's two boards), at real time, 3 turns, 25 min each
#   K  w06's fire blocks: the intro frame's --drawlog and --dumptex
#   P  exec tools/m39c_perf.sh (copied to ~/m39c_perf.sh): the fps-boards
#      measurements, the resyncs' named snapshots, then the leave-behind
#   L  exec the leave-behind soak: --board 1+ (the next board every chain)
# A fault stops the chain before the next board (no leave-behind), so the
# G4 is left at the fault's log for its reproduction.  Nothing is killed by
# name: a run is waited for, or killed by its own pid.
cd "$HOME"
sleep "${M39C_SETTLE:-10}"
APP="$HOME/MarioParty4.app/Contents/MacOS/isle"
D="$HOME/m39c"; mkdir -p "$D"
IDX="$D/index.txt"
PHASES=${PHASES:-B C D L}
echo "# m39c chain start $(date)  isle md5 $(md5 -q "$APP")  phases $PHASES" >> "$IDX"

faulted() { grep -q -E '^\*\*\* port: |port: fatal' "$1" 2>/dev/null; }

# run NAME CEILING_S LOGFILE args... : the game in the background, killed by pid
# past the ceiling or when it puts up port_fatal's dialog (which hangs a chain)
run() {
    name=$1; ceiling=$2; log=$3; shift 3
    t0=$(date +%s)
    "$APP" "$@" > "$log" 2>&1 &
    pid=$!
    while kill -0 $pid 2>/dev/null; do
        sleep 5
        if [ $(( $(date +%s) - t0 )) -gt "$ceiling" ]; then
            echo "m39c: $name over the $ceiling s ceiling -- killed" >> "$log"; kill -9 $pid
        elif grep -q "port: fatal" "$log" 2>/dev/null; then
            sleep 4; echo "m39c: $name port: fatal -- killed" >> "$log"; kill -9 $pid 2>/dev/null
        fi
    done
    wait $pid; RC=$?
}
stop_if_fault() {
    if faulted "$1"; then
        echo "m39c: FAULT in $2 -- chain stopped $(date)" >> "$IDX"
        echo "DONE (fault)" >> "$IDX"
        exit 1
    fi
}
md5s() {
    m=""; for f in 800 3000 7000; do p=$1/frame-0$f.ppm; [ $f -lt 1000 ] && p=$1/frame-00$f.ppm
        [ -f $p ] && h=$(md5 -q $p | cut -c1-8) || h=--------; m="$m $f:$h"; done; echo "$m"
}

for phase in $PHASES; do
case $phase in
A)
    WALK="--com4 --rtc dolphin --freshcard --noconfig --status --ovllog --perf --dumpframe 800,3000,7000 --turbo --play board-start-com4.play --frames 9000"
    for arm in M N MB1 NB1 NB2; do
        extra=""
        case $arm in
            N) extra="--nomovies" ;;
            MB1) extra="--board 1" ;;
            NB1) extra="--nomovies --board 1 --boarddump 0" ;;
            NB2) extra="--nomovies --board 2 --boarddump 0" ;;
        esac
        mkdir -p "$D/$arm"; rm -f "$D/$arm"/*.ppm
        run $arm 900 "$D/$arm.log" $WALK --shotdir "$D/$arm" $extra
        echo "$arm EXIT=$RC md5$(md5s "$D/$arm") $extra $(date)" >> "$IDX"
        stop_if_fault "$D/$arm.log" $arm
        sleep 5
    done
    ;;
B)
    # the board's entry under --nomovies at lockstep, from NB1's own log
    E=$(sed -n 's/.*boarddump: entered board [a-z0-9]* at frame \([0-9]*\).*/\1/p' "$D/NB1.log" | head -1)
    [ -n "$E" ] || E=${BOARD_ENTRY:-6000}
    FF=$((E - 400))
    OFFS=""; o=0; while [ $o -le ${SURVEY_LEN:-20000} ]; do OFFS="$OFFS${OFFS:+,}$o"; o=$((o + 250)); done
    for n in ${SURVEY_BOARDS:-1 2 3 4 5 6}; do
        name=S$n; mkdir -p "$D/$name"; rm -f "$D/$name"/*.ppm
        run $name 3600 "$D/$name.log" --com4 --rtc dolphin --freshcard --noconfig --nomovies \
            --play board-start-com4.play --board $n --ffto $FF --lockstep \
            --frames $((E + ${SURVEY_LEN:-20000} + 200)) --boarddump $OFFS --shotdir "$D/$name" \
            --status --ovllog --stuckwatch 90
        echo "$name EXIT=$RC entry $E dumps $(ls "$D/$name" | wc -l) $(date)" >> "$IDX"
        stop_if_fault "$D/$name.log" $name
        sleep 5
    done
    ;;
C)
    for n in ${SOAK_BOARDS:-2 3 4 5 6}; do
        name=R$n; log="$D/$name.log"
        echo "m39c: $name start $(date)" >> "$IDX"
        "$APP" --soak --com4 --rtc dolphin --freshcard --status --perf --board $n --turns ${TURNS:-5} \
            --ovllog --dvdlog > "$log" 2>&1 &
        pid=$!; t0=$(date +%s); why=ceiling
        while kill -0 $pid 2>/dev/null; do
            sleep 20
            if faulted "$log"; then why=fault; sleep 6; break; fi
            if grep -q "status .* w0${n}dll " "$log" && tail -2 "$log" | grep -q "status .* modeseldll "; then
                why=board-over; break
            fi
            [ $(( $(date +%s) - t0 )) -gt ${SOAK_CEIL:-4200} ] && break
        done
        if kill -0 $pid 2>/dev/null; then
            kill -INT $pid
            i=0; while kill -0 $pid 2>/dev/null && [ $i -lt 60 ]; do sleep 2; i=$((i+1)); done
            kill -0 $pid 2>/dev/null && { echo "m39c: $name did not leave in 120 s -- killed" >> "$log"; kill -9 $pid; }
        fi
        wait $pid; RC=$?
        echo "$name EXIT=$RC $why after $(( $(date +%s) - t0 )) s $(date)" >> "$IDX"
        stop_if_fault "$log" $name
        sleep 10
    done
    ;;
D)
    for x in w10dll w20dll w21dll; do
        name=X$x; mkdir -p "$D/$name"; rm -f "$D/$name"/*.ppm
        # the tutorial is read by a player pressing A through Toad's windows:
        # the walk's A metronome (every 64 frames to 29,960) does it; the
        # Extra Room's boards at its minimum 10 turns (PLAN.md 54b.3)
        run $name ${EXTRA_CEIL:-1500} "$D/$name.log" --goto $x:0:0 --com4 --rtc dolphin --freshcard \
            --noconfig --turns 10 --status --perf --ovllog --stuckwatch 90 --play board-start-com4.play \
            --boarddump 300,1500,3000,6000,12000,24000 --shotdir "$D/$name"
        echo "$name EXIT=$RC $(date)" >> "$IDX"
        stop_if_fault "$D/$name.log" $name
        sleep 5
    done
    ;;
K)
    # w06's fire blocks (PLAN.md 54b.5): the intro frame's draws explained and
    # every texture it decodes written out, the render thread off (the probe's rule)
    mkdir -p "$D/claw"; rm -f "$D/claw"/*
    run claw 900 "$D/claw.log" --com4 --rtc dolphin --freshcard --noconfig --nomovies \
        --play board-start-com4.play --board 6 --ffto 5258 --frames 5364 --norenderthread \
        --drawlog 4000 --drawlog-at 5358 --dumptex --dumpframe 5358 --shotdir "$D/claw"
    echo "claw EXIT=$RC $(date)" >> "$IDX"
    ;;
P)
    echo "m39c: perf chain $(date)" >> "$IDX"
    exec sh "$HOME/m39c_perf.sh"
    ;;
L)
    echo "m39c: leave-behind start $(date)" >> "$IDX"
    echo "DONE" >> "$IDX"
    exec "$APP" --soak --com4 --rtc dolphin --freshcard --realtime --snap-every 5000 \
         --snap-keep 3 --status --ovllog --stuckwatch 200 --perf --board 1+
    ;;
esac
done
echo "DONE" >> "$IDX"
