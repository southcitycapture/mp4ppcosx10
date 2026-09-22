#!/bin/sh
# M36 (PLAN.md 51): the loader's A/B on the G4, from a COLD page cache each
# run.  Three arms -- neither (--noprefetch --resident 0), the prefetch alone
# (--resident 0), both (the defaults) -- three runs each, interleaved so a
# cut-short chain still has every arm; each run is the realtime soak's own
# command capped at 60,000 frames: six minigames dealt by the roulette
# (m412 m428 m420 m444 m423 m438, PLAN.md 51.1).  The purge is a read of
# ~2.1 GB of the gallery's snapshots (126 x 43 MB): the box has 1.5 GB, so every page of the disc
# image, the bundle and its rels is dropped (Leopard has no `purge`).
# ON THE G4 as the console runner's job (the chain app).
cd $HOME
APP=${AB_APP:-$HOME/MarioParty4.app}
D=${AB_DIR:-$HOME/m36/ab}; mkdir -p $D
SOAK="--soak --com4 --rtc dolphin --freshcard --noconfig --realtime --status --ovllog --stuckwatch 200 --perf --frames ${AB_FRAMES:-60000}"
purge() {
    # 2 GB of files that are not the game's: the page cache is 1.5 GB at most
    n=0
    for f in $(ls $HOME/gallery-m35g/*/snaps/*.snap 2>/dev/null | head -${AB_PURGE_FILES:-50}); do
        cat "$f" > /dev/null; n=$((n+1))
    done
    echo "purge: read $n gallery snapshots (43 MB each) $(date)"
}
echo "ab: start $(date) isle $(md5 -q $APP/Contents/MacOS/isle)" | tee -a $D/index.txt
# a "port: fatal" puts up a dialog and hangs the chain (witness 0x): watch, kill by pid
( while true; do
    cur=$(cat $D/current 2>/dev/null)
    if [ -n "$cur" ] && grep -q "port: fatal" $cur 2>/dev/null; then
      pid=$(ps ax | grep "[i]sle --soak" | awk '{print $1}' | head -1)
      if [ -n "$pid" ]; then sleep 4; kill -9 $pid; fi
    fi
    sleep 10
    grep -q "ab: all done" $D/index.txt 2>/dev/null && exit 0
  done ) &
for run in 1 2 3; do
  for arm in P0R0 P1R0 P1R1; do
    case $arm in
      P0R0) FL="--noprefetch --resident 0" ;;
      P1R0) FL="--resident 0" ;;
      P1R1) FL="" ;;
    esac
    purge | tee -a $D/index.txt
    sleep 5
    echo "ab: $arm$run start $(date)" | tee -a $D/index.txt
    echo $D/$arm$run.log > $D/current
    $APP/Contents/MacOS/isle $SOAK $FL --perfdump $D/$arm$run.csv > $D/$arm$run.log 2>&1
    echo "ab: $arm$run EXIT=$? $(date) resyncs $(grep -c 'realtime: resync' $D/$arm$run.log) slow $(grep -c 'DVD: read of' $D/$arm$run.log)" | tee -a $D/index.txt
    sleep 3; killall -9 isle 2>/dev/null; sleep 3
  done
done
rm -f $D/current
echo "ab: all done $(date)" | tee -a $D/index.txt
