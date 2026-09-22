#!/bin/sh
# M35 wave 2 on the G4: after the gallery chain, the m433 fault on the M34 build, the seven
# games the collision killed, and the --nodirty A/Bs.  Every run is watched: a "port: fatal"
# in its log (the dialog would hang the chain) kills it by pid.
cd $HOME
while ps aux | grep -v grep | grep -q "sh /Users/zach/gallery_chain.sh"; do sleep 20; done
D=$HOME/m35/wave2; mkdir -p $D
P="--turns 1 --com4 --rtc dolphin --freshcard --play board-start-com4.play --noconfig --lockstep --status --ovllog"
wrun() { # name app ceiling args...
  name=$1; app=$2; ceil=$3; shift 3
  mkdir -p $D/$name; echo "wave2: $name start $(date)"
  $HOME/$app/Contents/MacOS/isle "$@" --shotdir $D/$name > $D/$name.log 2>&1 &
  pid=$!; t0=$(date +%s)
  while kill -0 $pid 2>/dev/null; do
    sleep 5
    if grep -q "port: fatal" $D/$name.log 2>/dev/null; then sleep 3; kill -9 $pid 2>/dev/null; fi
    if [ $(( $(date +%s) - t0 )) -gt $ceil ]; then echo "wave2: $name over ceiling" >> $D/$name.log; kill -9 $pid 2>/dev/null; fi
  done
  wait $pid; e=$?
  echo "wave2: $name EXIT=$e fatal=$(grep -c 'port: fatal' $D/$name.log) frames=$(ls $D/$name/*.ppm 2>/dev/null | wc -l | tr -d ' ') $(date)" | tee -a $D/index.txt
  sleep 3
}
wrun m433-m34 MarioParty4-m34.app 600 --minigame m433 $P --ffto 14400 --frames 14700 --dumpframe 14537
wrun m433-new MarioParty4.app 600 --minigame m433 $P --ffto 14400 --frames 14700 --dumpframe 14537
wrun m433-new-nrt MarioParty4.app 600 --minigame m433 $P --ffto 14400 --frames 14700 --dumpframe 14537 --norenderthread
export GALLERY_APP=$HOME/MarioParty4.app GALLERY_DIR=$HOME/gallery-m35 GAMES="423 401 415 404 406 425"
sh $HOME/gallery_chain.sh
wrun m404-nodirty MarioParty4.app 600 --minigame m404 $P --ffto 15600 --frames 15690 --dumpframe 15677 --nodirty
wrun m415-nodirty MarioParty4.app 600 --minigame m415 $P --ffto 15600 --frames 15690 --dumpframe 15677 --nodirty
echo "wave2: all done $(date)"
