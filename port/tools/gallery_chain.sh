#!/bin/sh
# M26 (PLAN.md 41): the gallery chain -- every minigame the roulette can deal,
# teleported into from boot and photographed four times inside the module
# (entry+60, +400, +1200, +2300), plus the instruction card (absolute frames
# 14200/14300/14400 of the board-start-com4 walk, instDll = 14136..14470)
# and the results screen 40 frames after the module is left, when it is.
#
# Runs ON THE G4 as the console runner's job: install it as the chain app's
# executable (~/MarioParty4-chain.app/Contents/MacOS/isle), `g4 use
# MarioParty4-chain.app; g4 run`.  One game after another; a fault or a stall
# ends that game's run (--frames is the ceiling) and the chain goes on.
# Every game leaves ~/gallery/mNNN/{f*.ppm,snaps/} and ~/gallery/mNNN.log;
# ~/gallery/index.txt gets one line per game: exit code, entry/exit frames,
# and the count of port> lines that are not status/dumpframe/ovllog noise.
# GALLERY_APP / GALLERY_DIR / GALLERY_EXTRA override the bundle, the output dir and
# extra flags for every game (M26 ran it twice: the M25 build into ~/gallery, the
# fixed build into ~/gallery-fix, and diffed the frames).
cd "$HOME"
NEW="${GALLERY_APP:-$HOME/MarioParty4.app}/Contents/MacOS/isle"
D="${GALLERY_DIR:-$HOME/gallery}"; mkdir -p "$D"
P="--com4 --rtc dolphin --freshcard --play board-start-com4.play --noconfig"
IDX="$D/index.txt"
echo "# gallery chain start $(date)  isle md5 $(md5 -q "$NEW")" >> "$IDX"

run() {
    name=$1; shift
    mkdir -p "$D/$name/snaps"
    echo "chain: $name start $(date)"
    "$NEW" --minigame "$name" --turns 1 $P --ffto 14000 --lockstep --frames 20000 \
        --mgdump 60,400,1200,2300 --mgend 2600 --dumpframe 14200,14300,14400 \
        --shotdir "$D/$name" --snap-every 800 --snap-keep 2 --snap-dir "$D/$name/snaps" \
        --status --ovllog $GALLERY_EXTRA "$@" > "$D/$name.log" 2>&1
    e=$?
    echo "chain: $name done EXIT=$e $(date)"
    entry=$(grep 'mgdump: entered' "$D/$name.log" | sed 's/.*at frame \([0-9]*\).*/\1/' | head -1)
    left=$(grep 'mgdump: left' "$D/$name.log" | sed 's/.*at frame \([0-9]*\) (entry +\([0-9]*\)).*/\1 (+\2)/' | head -1)
    fault=$(grep -c '^\*\*\* port' "$D/$name.log")
    warn=$(grep '^port> ' "$D/$name.log" | grep -v 'status f\|--dumpframe\|frame [0-9]*: overlay\|mgdump\|roulette\|--minigame\|--com4\|texture\|tex-cache\|copy-read\|report\|^port> *$' | wc -l | tr -d ' ')
    frames=$(ls "$D/$name"/*.ppm 2>/dev/null | wc -l | tr -d ' ')
    echo "$name EXIT=$e entry=${entry:--} left=${left:--} fault=$fault warn=$warn frames=$frames extra='$*'" >> "$IDX"
    sleep 2; killall -9 isle 2>/dev/null; sleep 2
}

for n in 401 402 403 404 405 406 407 408 409 410 411 412 413 414 415 \
         416 417 418 419 420 421 422 423 424 \
         425 426 427 428 429 430 431 432 433 434 435 436 437 438 439 440 441 442 443 444 \
         445 446 447 448 449 450 451 452 453 454 455 456 457 458 459 460 461 462 463; do
    case $n in
        453) run m453 --dvdheap 5888 ;;   # no four-character cast fits the retail HEAP_DVD (PLAN.md 29)
        *)   run m$n ;;
    esac
done
echo "# gallery chain all done $(date)" >> "$IDX"
echo "chain: all done $(date)"
