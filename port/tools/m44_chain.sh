#!/bin/sh
# M44 (PLAN.md 59): the front end, and the water -- M43's chain (below) plus the
# compiler's arms (a whole bundle each), the PGO training runs and the
# vertex cache's per-array counts.  ON THE G4
# as the console runner's job (M40's chain shape: `cp tools/m44_chain.sh` over
# ~/MarioParty4-chain.app/Contents/MacOS/isle, `g4 use MarioParty4-chain.app;
# g4 run RUNS...`).  Waits M44_SETTLE (90) s first.  One binary
# (~/MarioParty4.app, or M44_APP); ~/m44/NAME.log, NAME.csv, NAME.sample.txt,
# ~/m44/index.txt.
#
#   S:GAME           GAME at real time (the scoreboard's teleport), `sample`d for
#                    M44_SECS (12) s from entry + M44_WAIT (8) s (cs: frame 3000)
#   D:GAME           GAME at --turbo (lockstep: every frame drawn), sampled
#   C:GAME           GAME at --turbo --nodraw (every frame consumed), sampled
#   G:GAME           GAME at real time with --gxsplit (the game thread's regions)
#   A:GAME:ARM:K     GAME at real time, arm (below), repeat K: the A/B
#   L:GAME:ARM       GAME in lockstep, the gallery's two frames (exactness)
#   N / M / NA:ARM / MA:ARM   the md5 walks (turbo; N --nomovies, M movies); NA/MA with ARM
#   E:GAME[:ARM]     GAME in lockstep with --endlog at entry +600/+900/+1200
#                    (every batch end of those drawn frames, tools/m37_ends.py)
#                    and --vprogstats
#   FB:R[,R...]      ~/fps_board.sh's runs R (its FB_* settings from
#                    ~/fps-board.env; no second settle)
#   SOAK:MIN         a realtime soak of MIN minutes
# GAME: mNNN, cs (the character select), bN (board N), title.
# ARM: @NAME[,flags] -- the bundle ~/mp4-NAME.app with the flags (M43: the
# compiler's variants, each a whole build); base (the defaults), old (the M44_APP_OLD binary, default the 0.9.10
# bundle ~/MarioParty4-m41.app, with its defaults) or a flag list with ','
# for ' ' (e.g. --nostripes).
cd "$HOME"
[ -f "$HOME/m44.env" ] && . "$HOME/m44.env"
sleep "${M44_SETTLE:-90}"
D="${M44_DIR:-$HOME/m44}"; mkdir -p "$D"
IDX="$D/index.txt"
APP="${M44_APP:-$HOME/MarioParty4.app/Contents/MacOS/isle}"
WALK="--com4 --rtc dolphin --freshcard --noconfig --status --ovllog --perf --dumpframe 800,3000,7000"
TURBO="--turbo --play board-start-com4.play --frames 9000"
BASE="--com4 --rtc dolphin --freshcard --noconfig --perf --status --ovllog --play board-start-com4.play --nomovies"
echo "# m44 chain start $(date)  isle md5 $(md5 -q "$APP")" >> "$IDX"

# sampler NAME PID MODE: wait for the scene, then `sample` the pid
sampler() {
    name=$1; pid=$2; mode=$3
    log="$D/$name.log"
    n=0
    while kill -0 $pid 2>/dev/null; do
        case $mode in
            cs) f=$(grep '^port> status f' "$log" | tail -1 | sed 's/^port> status f\([0-9]*\).*/\1/')
                [ -n "$f" ] && [ "$f" -ge 3000 ] && break ;;
            bd) f=$(grep '^port> status f' "$log" | tail -1 | sed 's/^port> status f\([0-9]*\).*/\1/')
                [ -n "$f" ] && [ "$f" -ge 8400 ] && break ;;
            *)  grep -q 'mgdump: entered minigame' "$log" && break ;;
        esac
        sleep 1; n=$((n+1)); [ $n -gt 900 ] && return
    done
    sleep "${M44_WAIT:-8}"
    kill -0 $pid 2>/dev/null || return
    grep '^port> status f' "$log" | tail -1 > "$D/$name.sample-from.txt"
    sample $pid "${M44_SECS:-12}" -file "$D/$name.sample.txt" >/dev/null 2>&1
    grep '^port> status f' "$log" | tail -1 >> "$D/$name.sample-from.txt"
}

run() {
    name=$1; ceiling=$2; smode=$3; shift 3
    RAPP="$APP"
    case $name in
        *-old-*|*-old) RAPP="${M44_APP_OLD:-$HOME/MarioParty4-m43.app/Contents/MacOS/isle}" ;;
        *-@*) b=${name#*-@}; b=${b%%[-,]*}; RAPP="$HOME/mp4-$b.app/Contents/MacOS/isle" ;;
    esac
    mkdir -p "$D/$name"
    rm -f "$D/$name"/*.ppm "$D/$name.sample.txt"
    echo "chain: $name start $(date)"
    t0=$(date +%s)
    "$RAPP" --shotdir "$D/$name" --perfdump "$D/$name.csv" "$@" > "$D/$name.log" 2>&1 &
    pid=$!
    [ "$smode" != "-" ] && sampler "$name" $pid "$smode"
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
    echo "$name EXIT=$e wall=$(( $(date +%s) - t0 ))s fault=$fault isle=$(md5 -q "$RAPP" | cut -c1-8) md5$m args='$*'" >> "$IDX"
    sleep 5
}

arm() {
    case $1 in
        base|old|"") echo "" ;;
        @*,*) echo "${1#*,}" | tr ',' ' ' ;;
        @*) echo "" ;;
        *) echo "$1" | tr ',' ' ' ;;
    esac
}

# scene GAME -> the teleport's arguments (the scoreboard's)
scene() {
    case $1 in
        cs)     echo "--frames 5000 --ffto 2700" ;;
        title)  echo "--frames 1800" ;;
        b[1-6]) echo "--board ${1#b} --ffto 7808 --frames 10508" ;;
        m453)   echo "--minigame $1 --turns 1 --ffto 14000 --frames 20000 --mgend 1800 --dvdheap 5888" ;;
        m*)     echo "--minigame $1 --turns 1 --ffto 14000 --frames 20000 --mgend 1800" ;;
    esac
}
smode() { case $1 in cs) echo cs ;; m*) echo mg ;; b*) echo bd ;; *) echo - ;; esac; }

[ $# -gt 0 ] && M44_RUNS="$*"
for r in ${M44_RUNS:-N}; do
    case $r in
        N)    run N 700 - $WALK $TURBO --nomovies ;;
        M)    run M 900 - $WALK $TURBO ;;
        NA:*) a=${r#NA:}; run "N-$a" 700 - $WALK $TURBO --nomovies $(arm $a) ;;
        MA:*) a=${r#MA:}; run "M-$a" 900 - $WALK $TURBO $(arm $a) ;;
        S:*)  g=${r#S:}; run "S-$g" 600 $(smode $g) $BASE --realtime $(scene $g) ;;
        D:*)  g=${r#D:}; run "D-$g" 900 $(smode $g) $BASE --turbo $(scene $g) ;;
        SA:*) g=$(echo $r | cut -d: -f2); a=$(echo $r | cut -d: -f3)
              run "SA-$g-$a" 600 $(smode $g) $BASE --realtime $(scene $g) $(arm $a) ;;
        C:*)  g=${r#C:}; run "C-$g" 900 $(smode $g) $BASE --turbo --nodraw $(scene $g) --mgend 3000 ;;
        G:*)  g=${r#G:}; run "G-$g" 600 - $BASE --realtime --gxsplit $(scene $g) ;;
        A:*)
            g=$(echo $r | cut -d: -f2); a=$(echo $r | cut -d: -f3); k=$(echo $r | cut -d: -f4)
            run "A-$g-$a-$k" 600 - $BASE --realtime $(scene $g) $(arm $a) ;;
        L:*)
            g=$(echo $r | cut -d: -f2); a=$(echo $r | cut -d: -f3)
            run "L-$g-$a" 600 - --com4 --rtc dolphin --freshcard --noconfig --status --ovllog \
                --play board-start-com4.play --nomovies --minigame $g --turns 1 --ffto 14000 \
                --lockstep --frames 20000 --mgend 1300 --mgdump 300,1200 $(arm $a) ;;
        W:*)
            # M44: the water -- GAME in lockstep, the console comparison's
            # three positions (+400 / +1,200 / +2,300 from the entry)
            g=$(echo $r | cut -d: -f2); a=$(echo $r | cut -d: -f3)
            run "W-$g-$a" 900 - --com4 --rtc dolphin --freshcard --noconfig --status --ovllog \
                --play board-start-com4.play --nomovies --minigame $g --turns 1 --ffto 14000 \
                --lockstep --frames 20000 --mgend 2400 --mgdump 400,1200,2300 --perf $(arm $a) ;;
        E:*)
            g=$(echo $r | cut -d: -f2); a=$(echo $r | cut -d: -f3)
            run "E-$g-${a:-base}" 600 - --com4 --rtc dolphin --freshcard --noconfig --status --ovllog \
                --play board-start-com4.play --nomovies --minigame $g --turns 1 --ffto 14000 \
                --lockstep --frames 20000 --mgend 1300 --endlog 15077,15377,15677 --vprogstats $(arm $a) ;;
        P:*)
            # PGO training: the -fprofile-generate bundle ~/mp4-pgogen.app at
            # --turbo (every frame drawn) through GAME; the counters merge
            # into ~/pgo at each clean exit (GCOV_PREFIX)
            g=${r#P:}
            export GCOV_PREFIX="$HOME/pgo" GCOV_PREFIX_STRIP=0
            run "P-$g-@pgogen" 1500 - $BASE --turbo $(scene $g)
            unset GCOV_PREFIX GCOV_PREFIX_STRIP ;;
        PR:*)
            # ... and at real time, sampled like S (the drawn/consumed mix)
            g=${r#PR:}
            export GCOV_PREFIX="$HOME/pgo" GCOV_PREFIX_STRIP=0
            run "PR-$g-@pgogen" 900 - $BASE --realtime $(scene $g)
            unset GCOV_PREFIX GCOV_PREFIX_STRIP ;;
        V:*)
            # the vertex cache's reasons per list (--vcache count --vcarr A,B)
            # at real time; V:GAME:A-B (default 600-1500 from the entry)
            g=$(echo $r | cut -d: -f2); w=$(echo $r | cut -d: -f3 | tr '-' ',')
            run "V-$g" 600 - $BASE --realtime --vcache count --vcarr ${w:-600,1500} $(scene $g) ;;
        DS:*)
            # the decode plans (--decodestats, which keeps the cache out of the
            # way: a measurement run) at real time from the entry
            g=${r#DS:}
            run "DS-$g" 600 - $BASE --realtime --decodestats $(scene $g) ;;
        K:*)
            # the performance counters (--pmc SET) on the measurement bundle
            # ~/mp4-pmc.app, in the window M44_PMCWIN from the entry
            # M44: K:GAME:SET[:BUNDLE[,flags]] -- the bundle ~/mp4-BUNDLE.app (default pmc)
            g=$(echo $r | cut -d: -f2); k=$(echo $r | cut -d: -f3); kb=$(echo $r | cut -d: -f4)
            kf=""; case $kb in *,*) kf=$(echo "${kb#*,}" | tr ',' ' '); kb=${kb%%,*} ;; esac
            case $g in cs) w=${M44_PMCWIN_CS:-3000,4800} ;; b*) w=${M44_PMCWIN_B:-8400,10400} ;; *) w=${M44_PMCWIN:-300,1500} ;; esac
            run "K-$g-$k-@${kb:-pmc}$(echo $kf | tr -d ' ')" 600 - $BASE --realtime --pmc $k --pmcwin $w $(scene $g) $kf ;;
        FB:*)
            FB_SETTLE=0 sh "$HOME/fps_board.sh" $(echo ${r#FB:} | tr ',' ' ') ;;
        PROOF:*)
            # M44 (PLAN.md 59.9): the workload the G4 locked up in -- the
            # scoreboard's front run (fps_board.sh's, the board load at 5,108
            # included), again and again for MIN minutes, each with its md5s
            mins=${r#PROOF:}; tp=$(( $(date +%s) + mins * 60 )); k=1
            while [ $(date +%s) -lt $tp ]; do
                run "proof-$k" 400 - --rtc dolphin --freshcard --noconfig --realtime --perf --status \
                    --ovllog --com4 --play board-start-com4.play --nomovies --frames 9000 \
                    --dumpframe 800,3000,7000
                k=$((k + 1))
            done ;;
        SOAK:*)
            mins=${r#SOAK:}
            run "soak-$mins" $((mins * 60 + 300)) - --soak --com4 --rtc dolphin --freshcard --status --perf \
                --stuckwatch 200 --ovllog --frames $((mins * 3596)) ;;
    esac
done
echo "DONE" >> "$IDX"
[ -n "$M44_LEAVE" ] && exec "$APP" $M44_LEAVE
exit 0
