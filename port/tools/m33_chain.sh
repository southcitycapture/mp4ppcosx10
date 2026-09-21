#!/bin/sh
# M33 (PLAN.md 48): the decode placement per drawn frame (--rtdecode auto),
# A/B'd against the fixed modes ON THE G4 as the console runner's job
# (install as ~/MarioParty4-chain.app's executable; `g4 use
# MarioParty4-chain.app; g4 run [RUNS...]`), the M29 chain's shape.
#
# One binary (~/MarioParty4.app).  Arms (the first character after T/R):
#   0 = the decode on the game thread (the inline twin)
#   2 = the decode on the render thread, joined at the retrace (M29's default)
#   A = auto: a share per drawn frame on the game thread (M33)
#   D = the build's default (no --rtdecode)
# `z` N adds --zprepass N (M33: the depth pre-pass for alpha-killed fragments).
# Modes: T = --turbo 9,000 frames (the md5s), R = --realtime 16,000 frames
# (presented fps; medians by port/tools/m33_perfstat.py).  A trailing letter
# after the arm is a repeat (RAa, RAb ...); `f` NN sets --rtauto-fit NN,
# `m` NN --rtauto-max 0.NN.
#
#   ~/m33/NAME.log, ~/m33/NAME/{frame-*.ppm,perf.csv}, ~/m33/md5s.txt,
#   ~/m33/index.txt (one line per run: exit code, wall seconds, the md5s)
cd "$HOME"
D="${M33_DIR:-$HOME/m33}"; mkdir -p "$D"
IDX="$D/index.txt"
COMMON="--com4 --rtc dolphin --freshcard --noconfig --status --ovllog --perf \
--dumpframe 800,3000,7000 --perfwin 700-870:title,2600-3600:charselect,6000-8900:board"
TURBO="--turbo --play board-start-com4.play --frames 9000"
REAL="--soak --realtime --frames 16000"
echo "# m33 chain start $(date)  isle md5 $(md5 -q "$HOME/MarioParty4.app/Contents/MacOS/isle")" >> "$IDX"

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
    a=$1; c=$(echo "$a" | cut -c1)
    case $c in A) f="--rtdecode auto" ;; D) f="" ;; *) f="--rtdecode $c" ;; esac
    case $a in *f[0-9]*) f="$f --rtauto-fit $(echo "$a" | sed 's/.*f\([0-9][0-9]*\).*/\1/')" ;; esac
    case $a in *m[0-9]*) f="$f --rtauto-max 0.$(echo "$a" | sed 's/.*m\([0-9][0-9]*\).*/\1/')" ;; esac
    case $a in *z[0-9]*) f="$f --zprepass $(echo "$a" | sed 's/.*z\([0-9]\).*/\1/')" ;; esac
    echo "$f"
}

[ $# -gt 0 ] && M33_RUNS="$*"
for r in ${M33_RUNS:-TA T2 T0 RA R2 RAa R2a RAb R2b}; do
    case $r in
        T[02AD]*) a=${r#T}; run $r MarioParty4.app 700 $TURBO $(arm_flags $a) ;;
        R[02AD]*) a=${r#R}; run $r MarioParty4.app 480 $REAL $(arm_flags $a) ;;
        *)      echo "chain: unknown run $r" ;;
    esac
done
echo "# m33 chain all done $(date)" >> "$IDX"
echo "chain: all done $(date)"
