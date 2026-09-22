#!/bin/sh
# M35 (PLAN.md 50): the md5s of the three read fixes (the caster stage's
# constant, the background quad, the dirty ranges), each way, ON THE G4 as the console runner's job (install as
# ~/MarioParty4-chain.app's executable; `g4 use MarioParty4-chain.app; g4 run
# [RUNS...]`), the M33 chain's shape.
#
# One binary (~/MarioParty4.app).  Runs:
#   TD     --turbo 9,000 frames, the build's defaults: the md5s at 800/3000/7000
#   TDz1   the same with --zprepass 1 (the ZCompLoc gate, the hardware's rule)
#   TDz2   the same with --zprepass 2 (the ungated rule M33 shipped off)
#   RD     --realtime 16,000 frames: presented fps (m33_perfstat.py reads it)
#   K2     --turbo 3,200 frames of board-start.play (one human) with --kbport 2
#          and --paddbg: the keyboard as controller 2, frames 1500-3200/25
#   K2p    the same with --kbport 2 and a --play that presses A on port 1 only
#
#   ~/m35/NAME.log, ~/m35/NAME/{frame-*.ppm,perf.csv}, ~/m35/md5s.txt,
#   ~/m35/index.txt (one line per run: exit code, wall seconds, the md5s)
cd "$HOME"
D="${M35_DIR:-$HOME/m35}"; mkdir -p "$D"
IDX="$D/index.txt"
COMMON="--com4 --rtc dolphin --freshcard --noconfig --status --ovllog --perf \
--dumpframe 800,3000,7000 --perfwin 700-870:title,2600-3600:charselect,6000-8900:board"
TURBO="--turbo --play board-start-com4.play --frames 9000"
REAL="--soak --realtime --frames 16000"
KEYB="--turbo --rtc dolphin --freshcard --noconfig --status --ovllog --play board-start.play --frames 3200 --kbport 2 --paddbg --dumpframe 1500-3200/25"
echo "# m35 chain start $(date)  isle md5 $(md5 -q "$HOME/MarioParty4.app/Contents/MacOS/isle")" >> "$IDX"

run() {
    name=$1; app=$2; ceiling=$3; shift 3
    mkdir -p "$D/$name"
    rm -f "$D/$name"/*.ppm
    echo "chain: $name start $(date)"
    t0=$(date +%s)
    "$HOME/$app/Contents/MacOS/isle" --shotdir "$D/$name" --perfdump "$D/$name/perf.csv" "$@" > "$D/$name.log" 2>&1 &
    pid=$!
    while kill -0 $pid 2>/dev/null; do
        sleep 5
        if [ $(( $(date +%s) - t0 )) -gt "$ceiling" ]; then
            echo "chain: $name over the $ceiling s ceiling -- killed" | tee -a "$D/$name.log"
            kill -9 $pid 2>/dev/null
        fi
    done
    wait $pid; e=$?
    t1=$(date +%s)
    m=""
    for f in 800 3000 7000; do
        p="$D/$name/frame-0$f.ppm"
        [ $f -lt 1000 ] && p="$D/$name/frame-00$f.ppm"
        if [ -f "$p" ]; then
            h=$(md5 -q "$p" | cut -c1-8)
        else
            h="--------"
        fi
        m="$m $f:$h"
        echo "$name $f $h" >> "$D/md5s.txt"
    done
    echo "$name EXIT=$e wall=$((t1 - t0))s md5$m args='$*'" >> "$IDX"
    echo "chain: $name done EXIT=$e $(date)"
    sleep 2; killall -9 isle 2>/dev/null; sleep 3
}

[ $# -gt 0 ] && M35_RUNS="$*"
for r in ${M35_RUNS:-TD TDold}; do
    case $r in
        TD)    run $r MarioParty4.app 700 $COMMON $TURBO ;;
        TDold) run $r MarioParty4.app 700 $COMMON $TURBO --oldakonst --clrasclr --nodirty ;;
        TDak)  run $r MarioParty4.app 700 $COMMON $TURBO --oldakonst ;;
        TDcc)  run $r MarioParty4.app 700 $COMMON $TURBO --clrasclr ;;
        RD)    run $r MarioParty4.app 480 $COMMON $REAL ;;
        K2)    run $r MarioParty4.app 400 $KEYB ;;
        *)     echo "chain: unknown run $r" ;;
    esac
done
echo "# m35 chain all done $(date)" >> "$IDX"
echo "chain: all done $(date)"
