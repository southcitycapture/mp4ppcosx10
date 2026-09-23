#!/bin/sh
# M40 (PLAN.md 55): the vertex cache's checks and A/B, ON THE G4 as the console
# runner's job (the M39 chain's shape: `cp tools/m40_chain.sh` over
# ~/MarioParty4-chain.app/Contents/MacOS/isle, `g4 use MarioParty4-chain.app;
# g4 run RUNS...`).  Waits M40_SETTLE (90) s first.  One binary
# (~/MarioParty4.app); ~/m40/NAME.log, NAME.csv, NAME/*.ppm, ~/m40/index.txt.
#
#   N / NN / NV / NO / NC   the md5 walk, turbo, --nomovies: auto (the default) / on /
#                    on in a buffer object / --novcache / count
#   M                the md5 walk, turbo, movies on (cache on)
#   W / WO           the md5 walk at real time, --nomovies: on / off
#   A:GAME:ARM:K     GAME (m431, b4, cs = the character select, title) at real
#                    time, arm auto|on|vbo|avbo|off|count, repeat K (the A/B's runs)
#   P:GAME           GAME at real time, --novcache --gxsplit (the game thread's slices)
#   SOAK:MIN         a realtime soak of MIN minutes (the pauses' count)
cd "$HOME"
[ -f "$HOME/m40.env" ] && . "$HOME/m40.env"
sleep "${M40_SETTLE:-90}"
D="${M40_DIR:-$HOME/m40}"; mkdir -p "$D"
IDX="$D/index.txt"
APP="$HOME/MarioParty4.app/Contents/MacOS/isle"
WALK="--com4 --rtc dolphin --freshcard --noconfig --status --ovllog --perf --dumpframe 800,3000,7000"
TURBO="--turbo --play board-start-com4.play --frames 9000"
RT="--com4 --rtc dolphin --freshcard --noconfig --realtime --perf --status --ovllog --play board-start-com4.play --nomovies"
echo "# m40 chain start $(date)  isle md5 $(md5 -q "$APP")" >> "$IDX"

run() {
    name=$1; ceiling=$2; shift 2
    mkdir -p "$D/$name"
    rm -f "$D/$name"/*.ppm
    echo "chain: $name start $(date)"
    t0=$(date +%s)
    "$APP" --shotdir "$D/$name" --perfdump "$D/$name.csv" "$@" > "$D/$name.log" 2>&1 &
    pid=$!
    while kill -0 $pid 2>/dev/null; do
        sleep 5
        if [ $(( $(date +%s) - t0 )) -gt "$ceiling" ]; then
            echo "chain: $name over the $ceiling s ceiling -- killed" >> "$D/$name.log"
            kill -9 $pid 2>/dev/null
        fi
    done
    wait $pid; e=$?
    m=""
    for f in "$D/$name"/*.ppm; do
        [ -f "$f" ] && m="$m $(basename "$f" .ppm | sed 's/frame-0*//'):$(md5 -q "$f" | cut -c1-8)"
    done
    fault=$(grep -c '^\*\*\* port' "$D/$name.log")
    vc=$(grep 'port> vcache (M40' "$D/$name.log" | sed 's/.*hits (/hits (/' | cut -c1-80)
    echo "$name EXIT=$e wall=$(( $(date +%s) - t0 ))s fault=$fault md5$m vc='$vc' args='$*'" >> "$IDX"
    sleep 5
}

arm() {
    case $1 in
        auto)  echo "" ;;                              # the default
        on)    echo "--vcache on" ;;                   # every frame keyed, the vertex range
        vbo)   echo "--vcache on --vcachevbo" ;;       # every frame keyed, a buffer object
        avbo)  echo "--vcachevbo" ;;                   # auto, a buffer object
        off)   echo "--novcache" ;;
        count) echo "--vcache count" ;;
    esac
}

[ $# -gt 0 ] && M40_RUNS="$*"
for r in ${M40_RUNS:-N NO}; do
    case $r in
        N)   run N 700 $WALK $TURBO --nomovies ;;
        NN)  run NN 700 $WALK $TURBO --nomovies --vcache on ;;
        NV)  run NV 700 $WALK $TURBO --nomovies --vcache on --vcachevbo ;;
        MV)  run MV 900 $WALK $TURBO --vcache on --vcachevbo ;;
        NO)  run NO 700 $WALK $TURBO --nomovies --novcache ;;
        NC)  run NC 700 $WALK $TURBO --nomovies --vcache count ;;
        M)   run M 900 $WALK $TURBO ;;
        W)   run W 900 $WALK --realtime --play board-start-com4.play --frames 9000 --nomovies ;;
        WO)  run WO 900 $WALK --realtime --play board-start-com4.play --frames 9000 --nomovies --novcache ;;
        A:*)
            g=$(echo $r | cut -d: -f2); a=$(echo $r | cut -d: -f3); k=$(echo $r | cut -d: -f4)
            x=$(arm $a)
            case $g in
                cs)    run "A-$g-$a-$k" 600 $RT --frames 5000 --ffto 2700 $x ;;
                title) run "A-$g-$a-$k" 300 $RT --frames 1800 $x ;;
                b[1-6]) run "A-$g-$a-$k" 600 $RT --board ${g#b} --ffto 7808 --frames 10508 $x ;;
                m453)  run "A-$g-$a-$k" 420 $RT --minigame $g --turns 1 --ffto 14000 --frames 20000 --mgend 1800 --dvdheap 5888 $x ;;
                m*)    run "A-$g-$a-$k" 420 $RT --minigame $g --turns 1 --ffto 14000 --frames 20000 --mgend 1800 $x ;;
            esac ;;
        P:*)
            # the game thread's drawn frame by region (--gxsplit), the old path
            g=${r#P:}
            case $g in
                cs) run "P-$g" 600 $RT --frames 5000 --ffto 2700 --novcache --gxsplit ;;
                *)  run "P-$g" 420 $RT --minigame $g --turns 1 --ffto 14000 --frames 20000 --mgend 1800 --novcache --gxsplit ;;
            esac ;;
        SOAK:*)
            mins=${r#SOAK:}
            run "soak-$mins" $((mins * 60 + 300)) --soak --com4 --rtc dolphin --freshcard --status --perf \
                --stuckwatch 200 --ovllog --frames $((mins * 3596)) ;;
    esac
done
echo "DONE" >> "$IDX"
[ -n "$M40_LEAVE" ] && exec "$APP" $M40_LEAVE
exit 0
