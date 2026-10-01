#!/bin/sh
# M38 (PLAN.md 53): the movies, ON THE G4 as the console runner's job
# (`cp tools/m38_chain.sh` into ~/MarioParty4-chain.app/Contents/MacOS/isle,
# `g4 use MarioParty4-chain.app; g4 run [RUNS...]`), the M37 chain's shape.
#
# One binary (~/MarioParty4.app).  Runs:
#
#   N    the md5 walk with --nomovies: must give M37's 800/3000/7000 exactly
#   M    the md5 walk with the movies: the re-based references
#   B    the opening from boot at real time, no input: a frame every 300 from
#        600 to 5400 dumped (a dumped frame waits for its decode, the rest do
#        not), the movie's own log line
#   Q    the same without the dumps (the clean presented-fps and drop count)
#   BN   the same boot with --nomovies (the audio-underrun baseline)
#   Y    B with --thpyuv (the game's TEV through the port's GX: the A/B)
#   S    Q with --threads 0 --renderthread 1 (one CPU's shape on this one)
#   W    the md5 walk at real time with movies (--soak --realtime, 16000):
#        the two mode-select movies at the cap, logged
#   G    --goto mstory2dll:4:0, Mario's story ending (endmov_ma0.thp) at real
#        time: its movie's log line, frames every 600 to 9000
#   M2   M again: the re-based md5s are the same run to run
#   WSN  WS with --nomovies: the one-CPU walk's own underruns
#   C    --goto staffdll: the staff credits (stmov_a00.thp, stopped and
#        restarted by the module: HuTHPStop / HuTHPRestart)
#   WS   W with --threads 0 --renderthread 1: the mode-select movies in one
#        CPU's shape
#   A    Q with --wav: the mixed output, for the audio verdict against the
#        movie's own track (PLAN.md 53.5)
#   F    m448 with --foldcap 9: the felt's bisect, six caps in six frames
#   F2   F with --foldcap 19: the same six caps with the last alpha forced to 1
#   F21/F22  unit B's alpha from the crossbar alone / the constant alone
#   F3   m448 as a player gets it (the fold without the crossbar, 53.10)
#   F3X  the same with --foldxbar (the old fold: the felt black)
#   FD   the felt's --drawlog and --gltrace at 14117 on this schedule
#   L    m432 with --drawlog at the wall's frame: chan0's ambient and lights
#        (--nomovies: M37's schedule, so 14877 is M37's frame -- with the
#        movies the walk reaches m432 at 13251, inside --ffto 14000)
#
#   ~/m38/NAME.log, ~/m38/NAME/frame-*.ppm, ~/m38/index.txt
cd "$HOME"
D="${M38_DIR:-$HOME/m38}"; mkdir -p "$D"
IDX="$D/index.txt"
WALK="--com4 --rtc dolphin --freshcard --noconfig --status --ovllog --perf \
--dumpframe 800,3000,7000"
TURBO="--turbo --play board-start-com4.play --frames 9000"
BOOTSEQ="600,900,1200,1500,1800,2100,2400,2700,3000,3300,3600,3900,4200,4500,4800,5100,5400"
BOOT="--rtc dolphin --noconfig --status --frames 5600"
MG="--com4 --rtc dolphin --freshcard --play board-start-com4.play --noconfig --lockstep --status \
--ovllog --ffto 14000 --frames 20000"
echo "# m38 chain start $(date)  isle md5 $(md5 -q "$HOME/MarioParty4.app/Contents/MacOS/isle")" >> "$IDX"

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
    echo "$name EXIT=$e wall=$((t1 - t0))s md5$m movies=$thp args='$*'" >> "$IDX"
    echo "chain: $name done EXIT=$e $(date)"
    sleep 5
}

[ $# -gt 0 ] && M38_RUNS="$*"
for r in ${M38_RUNS:-N M}; do
    case $r in
        N)  run N 700 $WALK $TURBO --nomovies ;;
        M)  run M 900 $WALK $TURBO ;;
        B)  run B 400 $BOOT --dumpframe $BOOTSEQ ;;
        Q)  run Q 400 $BOOT ;;
        BN) run BN 400 $BOOT --nomovies ;;
        Y)  run Y 400 $BOOT --thpyuv --dumpframe 1200,2400,3600 ;;
        S)  run S 400 $BOOT --threads 0 --renderthread 1 ;;
        W)  run W 900 $WALK --soak --realtime --frames 16000 ;;
        G)  run G 400 --rtc dolphin --noconfig --status --goto mstory2dll:4:0 --play ending-a.play --frames 12000 \
                --thplog --dumpframe 3000,4000,5000,6000,7000,8000,9000,10000,11000 ;;
        M2) run M2 900 $WALK $TURBO ;;
        WSN) run WSN 900 $WALK --soak --realtime --frames 16000 --threads 0 --renderthread 1 --nomovies ;;
        C)  run C 900 --rtc dolphin --noconfig --status --goto staffdll:0:0 --play ending-a.play \
                --frames 30000 --thplog --dumpframe 2000,10000 ;;
        WS) run WS 900 $WALK --soak --realtime --frames 16000 --threads 0 --renderthread 1 ;;
        A)  run A 400 $BOOT --wav "$D/A.wav" ;;
        F)  run F 900 --minigame m448 --turns 1 $MG --mgdump 1200,1201,1202,1203,1204,1205 \
                --mgend 1260 --foldcap 9 ;;
        F2) run F2 900 --minigame m448 --turns 1 $MG --mgdump 1200,1201,1202,1203,1204,1205 \
                --mgend 1260 --foldcap 19 ;;
        F21) run F21 900 --minigame m448 --turns 1 $MG --mgdump 1200 --mgend 1260 --foldcap 21 ;;
        F22) run F22 900 --minigame m448 --turns 1 $MG --mgdump 1200 --mgend 1260 --foldcap 22 ;;
        F3) run F3 900 --minigame m448 --turns 1 $MG --mgdump 1200 --mgend 1260 ;;
        F3X) run F3X 900 --minigame m448 --turns 1 $MG --mgdump 1200 --mgend 1260 --foldxbar ;;
        FD) run FD 900 --minigame m448 --turns 1 $MG --mgdump 1200 --mgend 1260 --drawlog 3000 \
                --drawlog-at 14117 --gltrace 14117 ;;
        L)  run L 900 --minigame m432 --turns 1 $MG --drawlog 3000 --drawlog-at 14877 \
                --mgdump 400 --mgend 420 --nomovies ;;
        *)  echo "chain: unknown run $r" ;;
    esac
done
echo "# m38 chain all done $(date)" >> "$IDX"
echo "chain: all done $(date)"
