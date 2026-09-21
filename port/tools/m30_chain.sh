#!/bin/sh
# M30 (PLAN.md 45): the causes' witness chain ON THE G4 (install as
# ~/MarioParty4-chain.app's executable; `g4 use MarioParty4-chain.app; g4 run`).
#   T      the 9,000-frame turbo walk of PLAN.md 31.2 on ~/MarioParty4.app: the md5s
#   Told   the same with every M30 lever off (the pre-M30 picture on the same binary)
#   G      the gallery for the affected games (gallery_chain.sh, GAMES=...) into ~/gallery-m30
# ~/m30/NAME.log, ~/m30/NAME/frame-*.ppm, ~/m30/index.txt
cd "$HOME"
D="$HOME/m30"; mkdir -p "$D"
IDX="$D/index.txt"
COMMON="--com4 --rtc dolphin --freshcard --noconfig --status --ovllog --dumpframe 800,3000,7000"
TURBO="--turbo --play board-start-com4.play --frames 9000"
OLD="--nolinewidth --noregchain --oldfog --nospot --mdposonly"
echo "# m30 chain start $(date)  isle md5 $(md5 -q "$HOME/MarioParty4.app/Contents/MacOS/isle")" >> "$IDX"
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
[ $# -gt 0 ] && M30_RUNS="$*"
for r in ${M30_RUNS:-T Told G}; do
    case $r in
        T)    run T 700 $TURBO ;;
        Told) run Told 700 $TURBO $OLD ;;
        G)    GALLERY_DIR="$HOME/gallery-m30" GAMES="${M30_GAMES:-402 405 408 414 417 427 428 430 434 435 436 437 448 401 416}" \
                  sh "$HOME/MarioParty4-chain.app/Contents/Resources/gallery_chain.sh" ;;
    esac
done
echo "chain: all done $(date)"
