#!/bin/sh
# M39 final (PLAN.md 54): on the release candidate (~/MarioParty4.app): the two
# md5 walks (movies on, --nomovies), the whole gallery with --nomovies (the
# recipes are M37's schedule, PLAN.md 53.9), then exec the LONG soak with the
# player's defaults plus only --soak --com4 --rtc dolphin --freshcard --status
# --perf (no --snap-every: a player has no snapshots).  ON THE G4 as the
# console runner's job (`cp tools/m39_final.sh` into
# ~/MarioParty4-chain.app/Contents/MacOS/isle, `g4 use MarioParty4-chain.app;
# g4 run`); the soak's log is the runner's ~/isle-log.txt.  Nothing is killed
# by name: a run is waited for, or killed by its own pid at its ceiling.
cd "$HOME"
sleep "${M39_SETTLE:-90}"
APP="$HOME/MarioParty4.app/Contents/MacOS/isle"
D="$HOME/m39/final"; mkdir -p "$D"
G="${FINAL_GALLERY:-$HOME/gallery-m39}"; mkdir -p "$G"
echo "# m39 final start $(date)  isle md5 $(md5 -q "$APP")" >> "$D/index.txt"

# run NAME CEILING_S LOGFILE args... : the game in the background, killed by pid
# past the ceiling or when it puts up port_fatal's dialog (which hangs a chain)
run() {
    name=$1; ceiling=$2; log=$3; shift 3
    t0=$(date +%s)
    "$APP" "$@" > "$log" 2>&1 &
    pid=$!
    while kill -0 $pid 2>/dev/null; do
        sleep 5
        if [ $(( $(date +%s) - t0 )) -gt "$ceiling" ]; then
            echo "final: $name over the $ceiling s ceiling -- killed" >> "$log"; kill -9 $pid
        elif grep -q "port: fatal" "$log" 2>/dev/null; then
            sleep 4; echo "final: $name port: fatal -- killed" >> "$log"; kill -9 $pid 2>/dev/null
        fi
    done
    wait $pid; RC=$?
}
md5s() {
    m=""; for f in 800 3000 7000; do p=$1/frame-0$f.ppm; [ $f -lt 1000 ] && p=$1/frame-00$f.ppm
        [ -f $p ] && h=$(md5 -q $p | cut -c1-8) || h=--------; m="$m $f:$h"; done; echo "$m"
}
WALK="--com4 --rtc dolphin --freshcard --noconfig --status --ovllog --perf --dumpframe 800,3000,7000 --turbo --play board-start-com4.play --frames 9000"
for arm in M N; do
    extra=""; [ $arm = N ] && extra="--nomovies"
    mkdir -p "$D/$arm"; rm -f "$D/$arm"/*.ppm
    run $arm 900 "$D/$arm.log" $WALK --shotdir "$D/$arm" $extra
    echo "$arm EXIT=$RC md5$(md5s "$D/$arm") $extra $(date)" >> "$D/index.txt"
    sleep 5
done

# the gallery (gallery_chain.sh's run, pid-watched), every game with --nomovies
P="--com4 --rtc dolphin --freshcard --play board-start-com4.play --noconfig --nomovies"
IDX="$G/index.txt"
echo "# gallery chain start $(date)  isle md5 $(md5 -q "$APP")" >> "$IDX"
ALL="401 402 403 404 405 406 407 408 409 410 411 412 413 414 415 \
     416 417 418 419 420 421 422 423 424 \
     425 426 427 428 429 430 431 432 433 434 435 436 437 438 439 440 441 442 443 444 \
     445 446 447 448 449 450 451 452 453 454 455 456 457 458 459 460 461 462 463"
for n in ${GAMES:-$ALL}; do
    name=m$n; x=""; [ $n = 453 ] && x="--dvdheap 5888"
    mkdir -p "$G/$name/snaps"
    run $name 1500 "$G/$name.log" --minigame $name --turns 1 $P --ffto 14000 --lockstep --frames 20000 \
        --mgdump 60,400,1200,2300 --mgend 2600 --dumpframe 14200,14300,14400 \
        --shotdir "$G/$name" --snap-every 800 --snap-keep 2 --snap-dir "$G/$name/snaps" \
        --status --ovllog $x
    e=$RC
    entry=$(grep 'mgdump: entered' "$G/$name.log" | sed 's/.*at frame \([0-9]*\).*/\1/' | head -1)
    left=$(grep 'mgdump: left' "$G/$name.log" | sed 's/.*at frame \([0-9]*\) (entry +\([0-9]*\)).*/\1 (+\2)/' | head -1)
    fault=$(grep -c '^\*\*\* port' "$G/$name.log")
    frames=$(ls "$G/$name"/*.ppm 2>/dev/null | wc -l | tr -d ' ')
    echo "$name EXIT=$e entry=${entry:--} left=${left:--} fault=$fault frames=$frames extra='$x'" >> "$IDX"
    # m415's are kept: the one-CPU canvas finding's snapshot (PLAN.md 54.2)
    [ $n = 415 ] || rm -rf "$G/$name/snaps"
    sleep 3
done
echo "# gallery chain all done $(date)" >> "$IDX"
echo "final: gallery done $(date)" >> "$D/index.txt"
sleep 5
echo "final: long soak start $(date)" >> "$D/index.txt"
exec "$APP" --soak --com4 --rtc dolphin --freshcard --status --perf
