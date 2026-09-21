#!/bin/sh
# M31 (PLAN.md 46): the milestone's witness chain ON THE G4 (install as
# ~/MarioParty4-chain.app's executable; `g4 use MarioParty4-chain.app; g4 run`).
#   T      the 9,000-frame turbo walk of PLAN.md 31.2 on ~/MarioParty4.app: the md5s
#   Tnc    the same with --nocarry (the M31 TEV fold off; the other M31 fixes have no lever)
#   G      the gallery for the touched games (gallery_chain.sh, GAMES=...) into ~/gallery-m31
# ~/m30/NAME.log, ~/m30/NAME/frame-*.ppm, ~/m30/index.txt
cd "$HOME"
D="$HOME/m31"; mkdir -p "$D"
IDX="$D/index.txt"
COMMON="--com4 --rtc dolphin --freshcard --noconfig --status --ovllog --dumpframe 800,3000,7000"
TURBO="--turbo --play board-start-com4.play --frames 9000"
OLD="--nocarry"
echo "# m31 chain start $(date)  isle md5 $(md5 -q "$HOME/MarioParty4.app/Contents/MacOS/isle")" >> "$IDX"
run() {
    name=$1; ceiling=$2; shift 2
    mkdir -p "$D/$name"; rm -f "$D/$name"/*.ppm
    echo "chain: $name start $(date)"
    t0=$(date +%s)
    "$HOME/MarioParty4.app/Contents/MacOS/isle" $COMMON --shotdir "$D/$name" "$@" > "$D/$name.log" 2>&1 &
    pid=$!
    while kill -0 $pid 2>/dev/null; do
        sleep 5
        if [ $(( $(date +%s) - t0 )) -gt "$ceiling" ]; then
            echo "chain: $name over the $ceiling s ceiling -- killed" | tee -a "$D/$name.log"
            kill -9 $pid 2>/dev/null
        fi
    done
    wait $pid; e=$?
    m=""
    for f in 800 3000 7000; do
        p="$D/$name/frame-0$f.ppm"; [ $f -lt 1000 ] && p="$D/$name/frame-00$f.ppm"
        if [ -f "$p" ]; then h=$(md5 -q "$p" | cut -c1-8); else h="--------"; fi
        m="$m $f:$h"
    done
    echo "$name EXIT=$e wall=$(( $(date +%s) - t0 ))s md5$m args='$*'" >> "$IDX"
    echo "chain: $name done EXIT=$e $(date)"
    sleep 2; killall -9 isle 2>/dev/null; sleep 3
}
[ $# -gt 0 ] && M31_RUNS="$*"
for r in ${M31_RUNS:-T Tnc G}; do
    case $r in
        T)    run T 700 $TURBO ;;
        Tnc)  run Tnc 700 $TURBO $OLD ;;
        G)    GALLERY_DIR="$HOME/gallery-m31" GAMES="${M31_GAMES:-417 430 428 427 435 436 437 438 405 401 416}" \
                  sh "$HOME/MarioParty4-chain.app/Contents/Resources/gallery_chain.sh" ;;
    esac
done
echo "chain: all done $(date)"
