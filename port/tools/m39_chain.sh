#!/bin/sh
# M39 (PLAN.md 54): the release candidate, ON THE G4 as the console runner's
# job (`cp tools/m39_chain.sh` into ~/MarioParty4-chain.app/Contents/MacOS/isle,
# `g4 use MarioParty4-chain.app; g4 run [RUNS...]`), the M38 chain's shape.
# It waits 90 s first (the rule after g4_install.sh), M39_SETTLE to change.
#
# One binary (~/MarioParty4.app).  Runs (one-CPU = --threads 0 --renderthread 1):
#
#   WS    the md5 walk at real time, one CPU, the M39 defaults (the owed frame
#         decoded in the retrace's slack, the draw's audio guard at 60 ms)
#   WS0   the same with --nothpslice --thpguard 0: M38's one-CPU path
#   WSS   slices only (--thpguard 0)       WSG  guard only (--nothpslice)
#   WSN   WS with --nomovies: the one-CPU walk's own underruns
#   W     the md5 walk at real time, two CPUs (the regression check)
#   WN    W with --nomovies
#   S / S0   the opening from boot, one CPU, M39 / M38 paths
#   Q     the opening from boot, two CPUs
#   N / M    the md5 walks, turbo, --nomovies / movies
#
#   ~/m39/NAME.log, ~/m39/NAME/frame-*.ppm, ~/m39/index.txt
cd "$HOME"
sleep "${M39_SETTLE:-90}"
D="${M39_DIR:-$HOME/m39}"; mkdir -p "$D"
IDX="$D/index.txt"
WALK="--com4 --rtc dolphin --freshcard --noconfig --status --ovllog --perf \
--dumpframe 800,3000,7000"
TURBO="--turbo --play board-start-com4.play --frames 9000"
BOOTSEQ="600,900,1200,1500,1800,2100,2400,2700,3000,3300,3600,3900,4200,4500,4800,5100,5400"
BOOT="--rtc dolphin --noconfig --status --frames 5600"
MG="--com4 --rtc dolphin --freshcard --play board-start-com4.play --noconfig --lockstep --status \
--ovllog --ffto 14000 --frames 20000"
echo "# m39 chain start $(date)  isle md5 $(md5 -q "$HOME/MarioParty4.app/Contents/MacOS/isle")" >> "$IDX"

run() {
    name=$1; ceiling=$2; shift 2
    mkdir -p "$D/$name"
    rm -f "$D/$name"/*.ppm
    echo "chain: $name start $(date)"
    t0=$(date +%s)
    "$HOME/MarioParty4.app/Contents/MacOS/isle" --shotdir "$D/$name" "$@" > "$D/$name.log" 2>&1 &
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
    done
    thp=$(grep -c 'THP: the movie closes' "$D/$name.log")
    ur=$(grep "underrun(s) totalling" "$D/$name.log" | sed "s/.*out: //")
    echo "$name EXIT=$e wall=$((t1 - t0))s md5$m movies=$thp ur=\"$ur\" args='$*'" >> "$IDX"
    echo "chain: $name done EXIT=$e $(date)"
    sleep 5
}

[ $# -gt 0 ] && M39_RUNS="$*"
for r in ${M39_RUNS:-WS0 WS WSN}; do
    case $r in
        N)   run N 700 $WALK $TURBO --nomovies ;;
        M)   run M 900 $WALK $TURBO ;;
        Q)   run Q 400 $BOOT ;;
        S)   run S 400 $BOOT --threads 0 --renderthread 1 ;;
        S0)  run S0 400 $BOOT --threads 0 --renderthread 1 --nothpslice --thpguard 0 ;;
        W)   run W 900 $WALK --soak --realtime --frames 16000 ;;
        WN)  run WN 900 $WALK --soak --realtime --frames 16000 --nomovies ;;
        WS)  run WS 900 $WALK --soak --realtime --frames 16000 --threads 0 --renderthread 1 ;;
        WS0) run WS0 900 $WALK --soak --realtime --frames 16000 --threads 0 --renderthread 1 --nothpslice --thpguard 0 ;;
        WSS) run WSS 900 $WALK --soak --realtime --frames 16000 --threads 0 --renderthread 1 --thpguard 0 ;;
        WSG) run WSG 900 $WALK --soak --realtime --frames 16000 --threads 0 --renderthread 1 --nothpslice ;;
        WSN) run WSN 900 $WALK --soak --realtime --frames 16000 --threads 0 --renderthread 1 --nomovies ;;
        *)   echo "chain: unknown run $r" ;;
    esac
done
echo "# m39 chain all done $(date)" >> "$IDX"
echo "chain: all done $(date)"
