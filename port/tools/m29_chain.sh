#!/bin/sh
# M29 (PLAN.md 44): the decode on the render thread, its stages and walks,
# ON THE G4 as the console runner's job (install as ~/MarioParty4-chain.app's
# executable; `g4 use MarioParty4-chain.app; g4 run [RUNS...]`), so nothing on
# the host can interrupt it (g4-witness.md 0e).  Every run has a wall-clock
# ceiling.
#
# One binary (~/MarioParty4.app, the stages behind --rtdecode).  Arms:
#   0 = the decode on the game thread (the inline twin, M28's shape)
#   1 = the decode on the render thread, joined right after each record
#   2 = the decode on the render thread, joined at the retrace (the default)
# Modes: T = --turbo 9,000 frames (the md5s, the drawn frame), R = --realtime
# 16,000 frames (presented fps; medians by port/tools/m27_perfstat.py).
# Suffixes: c = --cpuskin (the skinning ordering under the game's own
# EnvelopeProc), i = --renderthread 1 (the inline replay twin).
# K = the results-screen stall's teleport (the M28 recipe on the M28 bundle,
# ~/MarioParty4-m28.app, with `sample` armed for frame 14,198).
#
#   ~/m29/NAME.log, ~/m29/NAME/{frame-*.ppm,perf.csv}, ~/m29/md5s.txt,
#   ~/m29/index.txt (one line per run: exit code, wall seconds, the md5s)
cd "$HOME"
D="${M29_DIR:-$HOME/m29}"; mkdir -p "$D"
IDX="$D/index.txt"
COMMON="--com4 --rtc dolphin --freshcard --noconfig --status --ovllog --perf --gxsplit \
--dumpframe 800,3000,7000 --perfwin 700-870:title,2600-3600:charselect,6000-8900:board"
TURBO="--turbo --play board-start-com4.play --frames 9000"
REAL="--soak --realtime --frames 16000"
echo "# m29 chain start $(date)  isle md5 $(md5 -q "$HOME/MarioParty4.app/Contents/MacOS/isle")" >> "$IDX"

run() {
    name=$1; app=$2; ceiling=$3; shift 3
    mkdir -p "$D/$name"
    rm -f "$D/$name"/*.ppm
    echo "chain: $name start $(date)"
    t0=$(date +%s)
    "$HOME/$app/Contents/MacOS/isle" $COMMON --shotdir "$D/$name" --perfdump "$D/$name/perf.csv" "$@" > "$D/$name.log" 2>&1 &
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

arm_flags() {
    a=$1; f="--rtdecode $(echo "$a" | cut -c1)"
    case $a in *c*) f="$f --cpuskin" ;; esac
    case $a in *i*) f="$f --renderthread 1" ;; esac
    echo "$f"
}

[ $# -gt 0 ] && M29_RUNS="$*"
for r in ${M29_RUNS:-T0 T1 T2 T2c T0c R2 R2a R2b R0}; do
    case $r in
        T[012]*) a=${r#T}; run $r MarioParty4.app 700 $TURBO $(arm_flags $a) ;;
        R[012]*) a=${r#R}; run $r MarioParty4.app 480 $REAL $(arm_flags $a) ;;
        M28R*)  # the M28 bundle at real time on today's machine (an environment check)
                run $r MarioParty4-m28.app 480 $REAL ;;
        M459*)  # m459's HEAP_HEAP exhaustion (PLAN.md 41) under a smaller stack multiplier
                m=$(echo "$r" | cut -c5-); [ -z "$m" ] && m=2
                run $r MarioParty4.app 400 --soak --realtime --minigame m459 --turns 1 --ffto 14000 --frames 17500 --stackmul $m ;;
        S459)   # the same, held for a screenshot from the host (g4 shot at ~15,500)
                run $r MarioParty4.app 400 --soak --realtime --minigame m459 --turns 1 --ffto 14000 --frames 16500 --stackmul 2 ;;
        K*)     # the results-screen stall (PLAN.md 43.12): the M28 bundle's own snapshot,
                # --realtime --perf, `sample` armed for the stall frame (14,198 on the walk)
                app=MarioParty4-m28.app; snap="$HOME/MarioParty4/snaps/lib/m28-results-stall-f014150.snap"
                echo "chain: $r start $(date)"; t0=$(date +%s); mkdir -p "$D/$r"
                "$HOME/$app/Contents/MacOS/isle" --restore "$snap" --restore-lax --com4 --rtc dolphin --noconfig --status --ovllog \
                    --play board-start-com4.play --realtime --perf --frames 14400 > "$D/$r.log" 2>&1 &
                pid=$!
                # arm: sample for 3 s at the 14,160 status line (one every 60 frames): the stall is at ~14,198
                while kill -0 $pid 2>/dev/null; do
                    sleep 0.3
                    if grep -q 'status f1416[0-9]\|status f142[0-9][0-9]' "$D/$r.log"; then break; fi
                    [ $(( $(date +%s) - t0 )) -gt 120 ] && break
                done
                sample isle 3 -file "$D/$r/stall.sample" > /dev/null 2>&1
                while kill -0 $pid 2>/dev/null; do sleep 2; [ $(( $(date +%s) - t0 )) -gt 200 ] && kill -9 $pid; done
                wait $pid; e=$?
                echo "$r EXIT=$e wall=$(( $(date +%s) - t0 ))s sample=$D/$r/stall.sample" >> "$IDX"
                echo "chain: $r done EXIT=$e $(date)"; killall -9 isle 2>/dev/null; sleep 3 ;;
        *)      echo "chain: unknown run $r" ;;
    esac
done
echo "# m29 chain all done $(date)" >> "$IDX"
echo "chain: all done $(date)"
