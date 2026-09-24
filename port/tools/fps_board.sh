#!/bin/sh
# M40 (PLAN.md 55): the 30-fps scoreboard's chain -- every screen a player can
# reach, at real time with --perf, one run per screen, for tools/fps_board.py.
# Runs ON THE G4 as the console runner's job: copy it over the chain app's
# executable (~/MarioParty4-chain.app/Contents/MacOS/isle), `g4 use
# MarioParty4-chain.app; g4 run [RUNS...]`.  It waits 90 s first (the rule
# after g4_install.sh; FB_SETTLE to change).  Re-runnable by any milestone.
#
#   FB_DIR    output directory            (default ~/fps-board)
#   FB_APP    the bundle under test       (default ~/MarioParty4.app)
#   FB_EXTRA  flags added to every run    (e.g. --novcache for the old path)
#   FB_GAMES  a subset of the minigames   (default all 63)
#   FB_MGLEN  frames of each minigame after its entry (default 1800)
#   RUNS      front title boards mg menus (default all five), or names such as
#             b4 m431 goto:option
#   FB_THREE  auto (default), all or off -- M42's three-run mode (PLAN.md 57.2):
#             a screen whose first run lands near the bar is run twice more
#             (NAME~2, NAME~3) and fps_board.py judges it by the MEDIAN OF THE
#             THREE RUNS' MEDIANS.  auto: the first run's median is within
#             FB_NEAR (2.0) fps of the bar (FB_BAR, 29.5) either side -- the
#             presented rate is capped at 30.0, so a screen AT the cap counts
#             as near only when its p10 is also within FB_NEAR of the bar (a
#             tail that one unlucky chain could turn into its median).  all:
#             every mg/board/goto run three times.  off: M40's one run.
#
# Every run: --realtime --perf --perfdump NAME.csv --status --ovllog, the
# player's defaults otherwise.  The walks are --nomovies so their frame
# numbers are the gallery's and the md5 walk's (the front run dumps 800/3000/
# 7000: the --nomovies references); the menus keep the movies (a player
# sees them).  Per minigame two frames are dumped (--mgdump 300,1200), so two
# arms of this chain can be compared frame by frame (the vertex cache's
# exactness, 63 games wide).  Each run is ended by its own --frames/--mgend or,
# past its ceiling, by its pid; ~/fps-board/index.txt gets a line per run.
#
#   port/tools/fps_board.py ~/fps-board/*.log --md port/docs/fps-scoreboard.md
cd "$HOME"
# the runner passes arguments but no environment: the settings may also come
# from ~/fps-board.env (sh syntax), which the lab writes before `g4 run`
[ -f "$HOME/fps-board.env" ] && . "$HOME/fps-board.env"
sleep "${FB_SETTLE:-90}"
APP="${FB_APP:-$HOME/MarioParty4.app}/Contents/MacOS/isle"
D="${FB_DIR:-$HOME/fps-board}"; mkdir -p "$D"
IDX="$D/index.txt"
X="${FB_EXTRA:-}"
MGLEN="${FB_MGLEN:-1800}"
RT="--rtc dolphin --freshcard --noconfig --realtime --perf --status --ovllog"
WALK="--com4 --play board-start-com4.play --nomovies"
E=5108   # the board's entry on the --nomovies walk (PLAN.md 54b.6)
echo "# fps-board start $(date)  isle md5 $(md5 -q "$APP")  extra '$X'  three ${FB_THREE:-auto}" >> "$IDX"

run() {
    name=$1; ceiling=$2; shift 2
    mkdir -p "$D/$name"
    rm -f "$D/$name"/*.ppm
    echo "fps-board: $name start $(date)"
    t0=$(date +%s)
    "$APP" --shotdir "$D/$name" --perfdump "$D/$name.csv" "$@" $X > "$D/$name.log" 2>&1 &
    pid=$!
    while kill -0 $pid 2>/dev/null; do
        sleep 5
        if [ $(( $(date +%s) - t0 )) -gt "$ceiling" ]; then
            echo "fps-board: $name over the $ceiling s ceiling -- killed" >> "$D/$name.log"
            kill -9 $pid 2>/dev/null
        fi
    done
    wait $pid; e=$?
    fault=$(grep -c '^\*\*\* port' "$D/$name.log")
    m=""
    for f in "$D/$name"/*.ppm; do
        [ -f "$f" ] && m="$m $(basename "$f" .ppm):$(md5 -q "$f" | cut -c1-8)"
    done
    echo "$name EXIT=$e wall=$(( $(date +%s) - t0 ))s fault=$fault md5$m args='$*'" >> "$IDX"
    sleep 3
}

# stats LOG SCREEN -> "median p10" of SCREEN's presented fps at real time
# (speed 99..110%, fps_board.py's rule), or nothing
stats() {
    grep '^port> status f' "$1" | awk -v s="$2" '$4 == s {
        sp = ""; fp = ""
        for (i = 5; i < NF; i++) {
            if ($i == "speed") sp = $(i + 1)
            if ($(i + 1) == "fps" && $(i + 2) == "presented") fp = $i
        }
        sub(/%/, "", sp)
        if (sp != "" && fp != "" && sp + 0 <= 110) print fp }' | sort -n | awk '
        { v[NR] = $1 }
        END { if (NR < 5) exit
              m = (NR % 2) ? v[(NR + 1) / 2] : (v[NR / 2] + v[NR / 2 + 1]) / 2
              k = (NR - 1) * 0.10; lo = int(k); hi = lo + 1; if (hi > NR - 1) hi = NR - 1
              p = v[lo + 1] + (v[hi + 1] - v[lo + 1]) * (k - lo)
              printf "%.2f %.2f\n", m, p }'
}

# near MEDIAN P10 -> exit 0 when the three-run rule wants two more runs
near() {
    awk -v m="$1" -v p="$2" -v b="${FB_BAR:-29.5}" -v w="${FB_NEAR:-2.0}" 'BEGIN {
        d = m - b; if (d < 0) d = -d
        if (d > w) exit 1                       # clearly short, or (never) far above
        if (m >= 30.0 && p >= b - w) exit 1     # at the cap with a tail out of reach
        exit 0 }'
}

# screen NAME SCREEN CEILING ARGS...: run NAME, and under the three-run rule
# NAME~2 and NAME~3 with the same arguments (their md5 dumps land in their
# own directories, so the gallery comparison keeps using NAME's)
screen() {
    base=$1; scr=$2; ceil=$3; shift 3
    run "$base" "$ceil" "$@"
    mode=${FB_THREE:-auto}
    [ "$mode" = off ] && return
    if [ "$mode" != all ]; then
        s=$(stats "$D/$base.log" "$scr")
        [ -z "$s" ] && return
        near $s || return
        echo "$base three-run: first median/p10 $s (within ${FB_NEAR:-2.0} of ${FB_BAR:-29.5})" >> "$IDX"
    fi
    run "$base~2" "$ceil" "$@"
    run "$base~3" "$ceil" "$@"
}

mg() {
    n=$1; shift
    screen m$n m${n}dll 420 $RT $WALK --minigame m$n --turns 1 --ffto 14000 --frames 20000 \
        --mgend "$MGLEN" --mgdump 300,1200 "$@"
}

ALLMG="401 402 403 404 405 406 407 408 409 410 411 412 413 414 415 \
       416 417 418 419 420 421 422 423 424 425 426 427 428 429 430 431 432 433 434 \
       435 436 437 438 439 440 441 442 443 444 445 446 447 448 449 450 451 453 455 \
       456 457 458 459 460 461 462 463"
MENUS="option present mpexdll mgmodedll mstorydll staffdll subchrseldll nisdll ztardll"

one() {
    case $1 in
        front)  run front 600 $RT $WALK --frames 9000 --dumpframe 800,3000,7000 ;;
        title)  run title 300 $RT --nomovies --frames 3600 ;;
        b[1-6]) n=${1#b}; screen b$n w0${n}dll 600 $RT $WALK --board $n --ffto $((E + 2700)) --frames $((E + 5400)) ;;
        m453)   mg 453 --dvdheap 5888 ;;   # the retail HEAP_DVD fits no four-character cast (PLAN.md 29)
        m4[0-9][0-9]) mg ${1#m} ;;
        goto:mstory2dll) screen goto-mstory2dll mstory2dll 300 $RT --goto mstory2dll:4:0 --play ending-a.play --frames 3600 ;;
        goto:*) o=${1#goto:}; screen goto-$o $o 300 $RT --goto "$o:0:0" --frames 2400 ;;
        boards) for b in 1 2 3 4 5 6; do one b$b; done ;;
        mg)     for g in ${FB_GAMES:-$ALLMG}; do one m$g; done ;;
        menus)  for o in $MENUS mstory2dll; do one goto:$o; done ;;
        *)      echo "fps-board: unknown run $1" >> "$IDX" ;;
    esac
}

[ $# -gt 0 ] && RUNS="$*"
for r in ${RUNS:-front title boards mg menus}; do
    one "$r"
done
echo "# fps-board all done $(date)" >> "$IDX"
echo "DONE" >> "$IDX"
[ -n "$FB_LEAVE" ] && exec "$APP" $FB_LEAVE
exit 0
