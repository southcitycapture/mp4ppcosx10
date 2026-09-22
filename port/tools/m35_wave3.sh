#!/bin/sh
# wave 3: m433 in the gallery's own configuration, twice (the fault of 18:38 came right after the
# --snap-every 800 write at 14400), with the fatal watcher; then the same with --norenderthread.
cd $HOME
while ! grep -q "wave2: all done" $HOME/m35/wave2.out 2>/dev/null; do sleep 20; done
D=$HOME/m35/wave3; mkdir -p $D
P="--minigame m433 --turns 1 --com4 --rtc dolphin --freshcard --play board-start-com4.play --noconfig --ffto 14000 --lockstep --frames 15000 --mgdump 60,400 --mgend 2600 --snap-every 800 --snap-keep 2 --status --ovllog"
wrun() { name=$1; shift
  mkdir -p $D/$name/snaps; echo "wave3: $name start $(date)"
  $HOME/MarioParty4.app/Contents/MacOS/isle $P --shotdir $D/$name --snap-dir $D/$name/snaps "$@" > $D/$name.log 2>&1 &
  pid=$!; t0=$(date +%s)
  while kill -0 $pid 2>/dev/null; do sleep 5
    grep -q "port: fatal" $D/$name.log 2>/dev/null && { sleep 3; kill -9 $pid 2>/dev/null; }
    [ $(( $(date +%s) - t0 )) -gt 600 ] && kill -9 $pid 2>/dev/null
  done
  wait $pid; e=$?
  echo "wave3: $name EXIT=$e fatal=$(grep -c 'port: fatal' $D/$name.log) frames=$(ls $D/$name/*.ppm 2>/dev/null | wc -l | tr -d ' ') $(date)" | tee -a $D/index.txt
  sleep 3
}
wrun g1
wrun g2
wrun g3-nrt --norenderthread
echo "wave3: all done $(date)"
