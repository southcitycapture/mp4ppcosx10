#!/bin/sh
# M35 final (second pass, PLAN.md 50.13-50.15): on the final build (~/MarioParty4.app): the md5 run, the whole gallery, then the
# realtime soak the milestone leaves running.  ON THE G4 as the console runner's job.
cd $HOME
D=${FINAL_DIR:-$HOME/m35/final2}; mkdir -p $D
COMMON="--com4 --rtc dolphin --freshcard --noconfig --status --ovllog --perf --dumpframe 800,3000,7000 --perfwin 700-870:title,2600-3600:charselect,6000-8900:board"
echo "final: TD start $(date)"
mkdir -p $D/TD; $HOME/MarioParty4.app/Contents/MacOS/isle $COMMON --turbo --play board-start-com4.play --frames 9000 --shotdir $D/TD --perfdump $D/TD/perf.csv > $D/TD.log 2>&1
m=""; for f in 800 3000 7000; do p=$D/TD/frame-0$f.ppm; [ $f -lt 1000 ] && p=$D/TD/frame-00$f.ppm; [ -f $p ] && h=$(md5 -q $p | cut -c1-8) || h=--------; m="$m $f:$h"; done
echo "TD EXIT=$? md5$m isle $(md5 -q $HOME/MarioParty4.app/Contents/MacOS/isle)" | tee -a $D/index.txt
sleep 2; killall -9 isle 2>/dev/null; sleep 2
echo "final: RD start $(date)"
mkdir -p $D/RD; $HOME/MarioParty4.app/Contents/MacOS/isle $COMMON --soak --realtime --frames 16000 --shotdir $D/RD --perfdump $D/RD/perf.csv > $D/RD.log 2>&1
echo "RD EXIT=$? $(date)" | tee -a $D/index.txt
sleep 2; killall -9 isle 2>/dev/null; sleep 2
export GALLERY_APP=$HOME/MarioParty4.app GALLERY_DIR=${FINAL_GALLERY:-$HOME/gallery-m35g}
# a "port: fatal" puts up a dialog and hangs the chain (m433, PLAN.md 50): watch the logs, kill by pid
( while true; do
    for l in $GALLERY_DIR/m4??.log; do
      if grep -q "port: fatal" "$l" 2>/dev/null; then
        n=$(basename "$l" .log)
        if ! grep -q "^$n EXIT" $GALLERY_DIR/index.txt 2>/dev/null; then
          sleep 4; pid=$(ps ax | grep "[i]sle --minigame $n " | awk '{print $1}' | head -1); [ -n "$pid" ] && kill -9 $pid
        fi
      fi
    done
    sleep 10
    grep -q "gallery chain all done" $GALLERY_DIR/index.txt 2>/dev/null && exit 0
  done ) &
sh $HOME/gallery_chain.sh
echo "final: gallery done $(date)" | tee -a $D/index.txt
sleep 3; killall -9 isle 2>/dev/null; sleep 3
echo "final: soak start $(date)" | tee -a $D/index.txt
exec $HOME/MarioParty4.app/Contents/MacOS/isle --soak --com4 --rtc dolphin --freshcard --realtime --snap-every 5000 --snap-keep 3 --status --ovllog --stuckwatch 200 --perf
