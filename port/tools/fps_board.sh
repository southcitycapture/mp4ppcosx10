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
echo "# fps-board start $(date)  isle md5 $(md5 -q "$APP")  extra '$X'" >> "$IDX"

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

mg() {
    n=$1; shift
    run m$n 420 $RT $WALK --minigame m$n --turns 1 --ffto 14000 --frames 20000 \
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
        b[1-6]) n=${1#b}; run b$n 600 $RT $WALK --board $n --ffto $((E + 2700)) --frames $((E + 5400)) ;;
        m453)   mg 453 --dvdheap 5888 ;;   # the retail HEAP_DVD fits no four-character cast (PLAN.md 29)
        m4[0-9][0-9]) mg ${1#m} ;;
        goto:mstory2dll) run goto-mstory2dll 300 $RT --goto mstory2dll:4:0 --play ending-a.play --frames 3600 ;;
        goto:*) o=${1#goto:}; run goto-$o 300 $RT --goto "$o:0:0" --frames 2400 ;;
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
