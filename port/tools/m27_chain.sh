#!/bin/sh
# M27 (PLAN.md 42): the render thread's witness chain -- the four stages on
# the 9,000-frame turbo walk (the three md5s each), then the 16,000-frame
# real-time walk in every mode (§39.4's table), one after another ON THE G4,
# as the console runner's job (install as ~/MarioParty4-chain.app's
# executable; `g4 use MarioParty4-chain.app; g4 run`), so nothing on the host
# can interrupt it (g4-witness.md 0e).  Every run has a wall-clock ceiling: a
# deadlocked render thread ends that run, not the chain.
#
#   ~/m27/NAME.log, ~/m27/NAME/frame-*.ppm, ~/m27/md5s.txt (one line per frame),
#   ~/m27/index.txt (one line per run: exit code, wall seconds, the md5s)
#
# M27_APP / M27_DIR / M27_RUNS override the bundle, the output dir and the
# run list ("T0 T1 T2 T3 R0 R3a R3b R3c R1 R2").
cd "$HOME"
APP="${M27_APP:-$HOME/MarioParty4.app}/Contents/MacOS/isle"
D="${M27_DIR:-$HOME/m27}"; mkdir -p "$D"
IDX="$D/index.txt"
COMMON="--com4 --rtc dolphin --freshcard --noconfig --status --ovllog --perf --gxsplit \
--dumpframe 800,3000,7000 --perfwin 700-870:title,2600-3600:charselect,6000-8900:board"
TURBO="--turbo --play board-start-com4.play --frames 9000"
REAL="--soak --realtime --frames 16000"
echo "# m27 chain start $(date)  isle md5 $(md5 -q "$APP")" >> "$IDX"

run() {
    name=$1; ceiling=$2; shift 2
    mkdir -p "$D/$name"
    rm -f "$D/$name"/*.ppm
    echo "chain: $name start $(date)"
    t0=$(date +%s)
    "$APP" $COMMON --shotdir "$D/$name" --perfdump "$D/$name/perf.csv" "$@" > "$D/$name.log" 2>&1 &
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

for r in ${M27_RUNS:-T0 T1 T2 T3 R0 R3a R3b R3c R1 R2}; do
    case $r in
        T0)  run T0 700 $TURBO --renderthread 0 ;;
        T1)  run T1 700 $TURBO --renderthread 1 ;;
        T2)  run T2 700 $TURBO --renderthread 2 ;;
        T3)  run T3 700 $TURBO --renderthread 3 ;;
        R0)  run R0 480 $REAL --renderthread 0 ;;
        R1)  run R1 480 $REAL --renderthread 1 ;;
        R2)  run R2 480 $REAL --renderthread 2 ;;
        R3*) run $r 480 $REAL --renderthread 3 ;;
        *)   echo "chain: unknown run $r" ;;
    esac
done
echo "# m27 chain all done $(date)" >> "$IDX"
echo "chain: all done $(date)"
