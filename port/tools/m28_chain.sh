#!/bin/sh
# M28 (PLAN.md 43): the experiments' A/B chain, ON THE G4 as the console
# runner's job (install as ~/MarioParty4-chain.app's executable; `g4 use
# MarioParty4-chain.app; g4 run [RUNS...]`), so nothing on the host can
# interrupt it (g4-witness.md 0e).  Every run has a wall-clock ceiling.
#
# One binary (~/MarioParty4.app, every experiment behind a flag) and two
# compiler bundles (~/MarioParty4-o3.app, ~/MarioParty4-fast.app, PLAN.md 43
# (e)).  Arms: 0 = every experiment off (the control, the M27 shape), a b c d
# = one on, 5 = all on (the defaults).  Modes: N = --nodraw --turbo 9,000
# frames (the consumed frame's game time), T = --turbo 9,000 frames (the
# md5s, the drawn frame), R = --realtime 16,000 frames (presented fps, the
# consumed and drawn frames' medians by port/tools/m27_perfstat.py).  S0/S5 =
# the snapdiff arms (--nodraw, a snapshot at frame 8,100 and the .wav, both
# with a runtime flag on one binary, so the .wav IS an oracle: PLAN.md 33.4).
# Q = port/tests/mtx_test --sqrt-all (every positive float through
# port_sqrtf and libm).
#
#   ~/m28/NAME.log, ~/m28/NAME/{frame-*.ppm,perf.csv}, ~/m28/md5s.txt,
#   ~/m28/index.txt (one line per run: exit code, wall seconds, the md5s)
cd "$HOME"
D="${M28_DIR:-$HOME/m28}"; mkdir -p "$D"
IDX="$D/index.txt"
COMMON="--com4 --rtc dolphin --freshcard --noconfig --status --ovllog --perf --gxsplit \
--dumpframe 800,3000,7000 --perfwin 700-870:title,2600-3600:charselect,6000-8900:board"
TURBO="--turbo --play board-start-com4.play --frames 9000"
NODRAW="--nodraw --turbo --play board-start-com4.play --frames 9000"
REAL="--soak --realtime --frames 16000"
OFF0="--nomatwalk --nocurvememo --nosparsemtx --nofastsqrt"
OFFa="--nocurvememo --nosparsemtx --nofastsqrt"
OFFb="--nomatwalk --nosparsemtx --nofastsqrt"
OFFc="--nomatwalk --nocurvememo --nofastsqrt"
OFFd="--nomatwalk --nocurvememo --nosparsemtx"
OFF5=""
echo "# m28 chain start $(date)  isle md5 $(md5 -q "$HOME/MarioParty4.app/Contents/MacOS/isle")" >> "$IDX"

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
    case $1 in
        0) echo "$OFF0" ;; a) echo "$OFFa" ;; b) echo "$OFFb" ;;
        c) echo "$OFFc" ;; d) echo "$OFFd" ;; 5) echo "$OFF5" ;;
    esac
}

[ $# -gt 0 ] && M28_RUNS="$*"
for r in ${M28_RUNS:-N0 N5 T0 T5 R0 R5 S0 S5 Na Nb Nc Ta Tb Tc Td Ra Rb Rc Rd O3T O3R FT FR Q}; do
    case $r in
        N[0abcd5]) a=${r#N}; run $r MarioParty4.app 400 $NODRAW $(arm_flags $a) ;;
        T[0abcd5]) a=${r#T}; run $r MarioParty4.app 700 $TURBO $(arm_flags $a) ;;
        R[0abcd5]*) a=$(echo "$r" | cut -c2); run $r MarioParty4.app 480 $REAL $(arm_flags $a) ;;
        W[05])  # the picture-history witness for (a): 17 frames forced drawn at
                # real time, each after a different run of consumed frames
                a=${r#W}; run $r MarioParty4.app 480 $REAL $(arm_flags $a) \
                    --dumpframe 800,1500,2500,3000,4000,5000,6500,7000,7500,8500,9500,10500,11500,12500,13500,14500,15500 ;;
        S[05])  a=${r#S}; run $r MarioParty4.app 400 --nodraw --turbo --play board-start-com4.play \
                    --frames 8101 --snap-at 8100 --snap-dir "$D/$r" --wav "$D/$r/walk.wav" $(arm_flags $a) ;;
        Rm*)    # (f): M22's batch merge (the same-state neighbours joined) with the
                # render thread on -- does fewer batches move the game thread now?
                run $r MarioParty4.app 480 $REAL --lazyflush --premerge-max 256 --submitstats ;;
        Tm)     run Tm MarioParty4.app 700 $TURBO --lazyflush --premerge-max 256 --submitstats ;;
        K)      # the results-screen stall's teleport point (PLAN.md 43.1): a snapshot
                # 50 frames before the walk's first minigame results (retrace 14,203)
                run K MarioParty4.app 400 --soak --realtime --ffto 14100 --frames 14400 \
                    --snap-at 14150 --snap-dir "$D/K" ;;
        O3T)    run O3T MarioParty4-o3.app 700 $TURBO ;;
        O3R*)   run $r MarioParty4-o3.app 480 $REAL ;;
        FT)     run FT MarioParty4-fast.app 700 $TURBO ;;
        FR*)    run $r MarioParty4-fast.app 480 $REAL ;;
        P[NT]|P[NT]-*)
                # the profile (PLAN.md 43.1): teleport to the board and `sample`
                # ten seconds of it; PN = --nodraw (a consumed frame), PT = drawn
                app=MarioParty4.app; case $r in *-old) app=MarioParty4-m27.app ;; esac
                # PN walks there under --nodraw (a --nodraw --ffto run drew past N until M28)
                nd="--ffto 6500"; [ "${r#PN}" != "$r" ] && nd="--nodraw"
                echo "chain: $r start $(date)"; t0=$(date +%s); mkdir -p "$D/$r"
                "$HOME/$app/Contents/MacOS/isle" --com4 --rtc dolphin --freshcard --noconfig --status --ovllog \
                    --play board-start-com4.play --turbo $nd > "$D/$r.log" 2>&1 &
                pid=$!
                while kill -0 $pid 2>/dev/null; do
                    sleep 3
                    if grep -q 'status f6[6-9][0-9][0-9][0-9]\|status f[7-9][0-9][0-9][0-9]' "$D/$r.log"; then break; fi
                    [ $(( $(date +%s) - t0 )) -gt 300 ] && break
                done
                sleep 4
                sample isle 10 -file "$D/$r/board.sample" > /dev/null 2>&1
                kill $pid 2>/dev/null; sleep 3; kill -9 $pid 2>/dev/null; wait $pid 2>/dev/null
                echo "$r EXIT=? wall=$(( $(date +%s) - t0 ))s sample=$D/$r/board.sample" >> "$IDX"
                echo "chain: $r done $(date)"; killall -9 isle 2>/dev/null; sleep 3 ;;
        Q*)     stride=${r#Q}; stride=${stride:-1}   # Q = every float, Q3 = every third
                echo "chain: $r start $(date)"; t0=$(date +%s)
                "$HOME/mtx_test_m28" --sqrt-all $stride > "$D/$r.log" 2>&1; e=$?
                echo "$r EXIT=$e wall=$(( $(date +%s) - t0 ))s" >> "$IDX"
                echo "chain: $r done EXIT=$e $(date)" ;;
        *)      echo "chain: unknown run $r" ;;
    esac
done
echo "# m28 chain all done $(date)" >> "$IDX"
echo "chain: all done $(date)"
