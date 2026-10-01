#!/bin/sh
# (port/tools/m34_m417ab.sh, run on the G4 as the chain app; PLAN.md 49.7)
# M34: m417's streaks at +1200 (frame 15677), three arms on the G4
cd "$HOME"; D=$HOME/m34; mkdir -p $D
R="--minigame m417 --turns 1 --com4 --rtc dolphin --freshcard --noconfig --play board-start-com4.play --ffto 15600 --frames 15700 --dumpframe 15677 --status --ovllog"
run() { name=$1; shift; mkdir -p $D/$name; rm -f $D/$name/*.ppm; echo "chain: $name start $(date)"
  "$HOME/MarioParty4.app/Contents/MacOS/isle" --shotdir $D/$name "$@" > $D/$name.log 2>&1; echo "chain: $name done EXIT=$? $(date)"
  sleep 2; killall -9 isle 2>/dev/null; sleep 2; }
for r in ${M417_RUNS:-S417 S417nrt S417noav}; do
  case $r in
    S417)     run $r $R ;;
    S417nrt)  run $r $R --norenderthread --lockstep ;;
    S417noav) run $r $R --noaltivec ;;
    S417cpu)  run $r $R --cpuxf ;;
    S417skip) run $r $R --skipobj luigi_m2 ;;
    S417dl)   run $r --minigame m417 --turns 1 --com4 --rtc dolphin --freshcard --noconfig --play board-start-com4.play --ffto 15600 --frames 15700 --dumpframe 15677 --status --norenderthread --lockstep --drawlog 400 --drawlog-at 15677 ;;
    S417sv)   run $r $R --skipverts 2048 ;;
    S417p)    run $r --minigame m417 --turns 1 --com4 --rtc dolphin --freshcard --noconfig --play board-start-com4.play --ffto 15676 --frames 15684 --dumpframe 15677 --status --norenderthread --lockstep --probeobj "*" --probebox 60,190,300,290 --probeverts 2100 ;;
  esac
done
echo "chain: all done $(date)"
