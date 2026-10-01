#!/bin/sh
# M42 (PLAN.md 57): the total work -- profiles, batch ends, A/B, ON THE G4
# as the console runner's job (M40's chain shape: `cp tools/m42_chain.sh` over
# ~/MarioParty4-chain.app/Contents/MacOS/isle, `g4 use MarioParty4-chain.app;
# g4 run RUNS...`).  Waits M42_SETTLE (90) s first.  One binary
# (~/MarioParty4.app, or M42_APP); ~/m42/NAME.log, NAME.csv, NAME.sample.txt,
# ~/m42/index.txt.
#
#   S:GAME           GAME at real time (the scoreboard's teleport), `sample`d for
#                    M42_SECS (12) s from entry + M42_WAIT (8) s (cs: frame 3000)
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
# ARM: base (the defaults), old (the M42_APP_OLD binary, default the 0.9.10
# bundle ~/MarioParty4-m41.app, with its defaults) or a flag list with ','
# for ' ' (e.g. --nostripes).
cd "$HOME"
[ -f "$HOME/m42.env" ] && . "$HOME/m42.env"
sleep "${M42_SETTLE:-90}"
D="${M42_DIR:-$HOME/m42}"; mkdir -p "$D"
IDX="$D/index.txt"
APP="${M42_APP:-$HOME/MarioParty4.app/Contents/MacOS/isle}"
WALK="--com4 --rtc dolphin --freshcard --noconfig --status --ovllog --perf --dumpframe 800,3000,7000"
TURBO="--turbo --play board-start-com4.play --frames 9000"
BASE="--com4 --rtc dolphin --freshcard --noconfig --perf --status --ovllog --play board-start-com4.play --nomovies"
echo "# m42 chain start $(date)  isle md5 $(md5 -q "$APP")" >> "$IDX"

# sampler NAME PID MODE: wait for the scene, then `sample` the pid
sampler() {
    name=$1; pid=$2; mode=$3
    log="$D/$name.log"
    n=0
    while kill -0 $pid 2>/dev/null; do
        case $mode in
            cs) f=$(grep '^port> status f' "$log" | tail -1 | sed 's/^port> status f\([0-9]*\).*/\1/')
                [ -n "$f" ] && [ "$f" -ge 3000 ] && break ;;
            *)  grep -q 'mgdump: entered minigame' "$log" && break ;;
        esac
        sleep 1; n=$((n+1)); [ $n -gt 900 ] && return
    done
    sleep "${M42_WAIT:-8}"
    kill -0 $pid 2>/dev/null || return
    grep '^port> status f' "$log" | tail -1 > "$D/$name.sample-from.txt"
    sample $pid "${M42_SECS:-12}" -file "$D/$name.sample.txt" >/dev/null 2>&1
    grep '^port> status f' "$log" | tail -1 >> "$D/$name.sample-from.txt"
}

run() {
    name=$1; ceiling=$2; smode=$3; shift 3
    RAPP="$APP"
    case $name in
        *-old-*) RAPP="${M42_APP_OLD:-$HOME/MarioParty4-m41.app/Contents/MacOS/isle}" ;;
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
smode() { case $1 in cs) echo cs ;; m*) echo mg ;; *) echo - ;; esac; }

[ $# -gt 0 ] && M42_RUNS="$*"
for r in ${M42_RUNS:-N}; do
    case $r in
        N)    run N 700 - $WALK $TURBO --nomovies ;;
        M)    run M 900 - $WALK $TURBO ;;
        NA:*) a=${r#NA:}; run "N-$a" 700 - $WALK $TURBO --nomovies $(arm $a) ;;
        MA:*) a=${r#MA:}; run "M-$a" 900 - $WALK $TURBO $(arm $a) ;;
        S:*)  g=${r#S:}; run "S-$g" 600 $(smode $g) $BASE --realtime $(scene $g) ;;
        D:*)  g=${r#D:}; run "D-$g" 900 $(smode $g) $BASE --turbo $(scene $g) ;;
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
        E:*)
            g=$(echo $r | cut -d: -f2); a=$(echo $r | cut -d: -f3)
            run "E-$g-${a:-base}" 600 - --com4 --rtc dolphin --freshcard --noconfig --status --ovllog \
                --play board-start-com4.play --nomovies --minigame $g --turns 1 --ffto 14000 \
                --lockstep --frames 20000 --mgend 1300 --endlog 15077,15377,15677 --vprogstats $(arm $a) ;;
        FB:*)
            FB_SETTLE=0 sh "$HOME/fps_board.sh" $(echo ${r#FB:} | tr ',' ' ') ;;
        SOAK:*)
            mins=${r#SOAK:}
            run "soak-$mins" $((mins * 60 + 300)) - --soak --com4 --rtc dolphin --freshcard --status --perf \
                --stuckwatch 200 --ovllog --frames $((mins * 3596)) ;;
    esac
done
echo "DONE" >> "$IDX"
[ -n "$M42_LEAVE" ] && exec "$APP" $M42_LEAVE
exit 0
