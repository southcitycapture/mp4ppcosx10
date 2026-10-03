#!/bin/sh
# M53 (PLAN.md 68): the m432 load-fault loop as the G4 ran it (~/m53rep4.sh): the
# scoreboard's teleport to Dungeon Duos, the arms interleaved, each run's faults and
# the M53 checks counted into ~/m53/rep-index.txt; yields to a Pikmin lab run.
# M53 m432 load-fault loop, interleaved arms.  Env: APP NAME N ; arms in ARMS (ARMa|ARMb ...), each "ENV;FLAGS"
APP=${APP:-$HOME/MarioParty4.app}/Contents/MacOS/isle
D=$HOME/m53/rep-$NAME; mkdir -p $D
I=$HOME/m53/rep-index.txt
echo "# $NAME start $(date) md5 $(md5 -q $APP) N=$N arms='$ARMS'" >> $I
RT="--rtc dolphin --freshcard --noconfig --realtime --perf --status --ovllog"
WALK="--com4 --play board-start-com4.play --nomovies"
i=1
while [ $i -le $N ]; do
  k=0
  echo "$ARMS" | tr '|' '\n' > $D/arms.txt
  while read arm; do
    [ -f $HOME/m53/stop ] && break 2
    # yield to another project's timed run on this machine (Pikmin's lab), at most 20 min
    w=0; while ps -axo comm | grep -q '^/Users/zach/pikmin/.*pikmin$\|/pikmin$' && [ $w -lt 1200 ]; do sleep 20; w=$((w+20)); done
    [ $w -gt 0 ] && echo "$NAME yielded ${w}s to a pikmin run" >> $I
    k=$((k+1)); ev=${arm%%;*}; fl=${arm#*;}
    t0=$(date +%s)
    env $ev "$APP" $RT $WALK --minigame m432 --turns 1 --ffto 14000 --frames ${FR:-14800} $fl > $D/r$i-$k.log 2>&1 &
    pid=$!
    while kill -0 $pid 2>/dev/null; do
      sleep 3
      [ $(( $(date +%s) - t0 )) -gt 240 ] && { echo "killed: ceiling" >> $D/r$i-$k.log; kill -9 $pid; }
    done
    wait $pid; e=$?
    f=$(grep -c '^\*\*\* port' $D/r$i-$k.log)
    c=$(grep -c 'CHECK FAILED\|RESIDENT COPY CHANGED\|WRITE INTO A DATA' $D/r$i-$k.log)
    echo "$NAME r$i-$k EXIT=$e fault=$f checks=$c wall=$(( $(date +%s) - t0 ))s arm='$arm'" >> $I
    if [ "$f" = 0 ] && [ "$e" = 0 ] && [ "$c" = 0 ]; then grep -v '^port> status\|^\[perf' $D/r$i-$k.log | tail -12 > $D/r$i-$k.tail; rm -f $D/r$i-$k.log; else gzip -f $D/r$i-$k.log; fi
    sleep 2
  done < $D/arms.txt
  i=$((i+1))
done
echo "# $NAME end $(date)" >> $I
