#!/bin/sh
# M37 (PLAN.md 52.8, 52.9): the warp shader's two unexplained behaviours,
# bounded to two runs, and the two singles -- m448's felt and m432's walls.
# M35's chain (tools/m35_tfs.sh) with the current build as its binary and
# ~/m37/tfs as its directory.  ON THE G4 as the console runner's job
# (install as ~/MarioParty4-chain.app's executable; `g4 use
# MarioParty4-chain.app; g4 run [RUNS...]`).
#
#   p25      m434's warp draw with --tfsdbg 25: PassTexCoord t0 as the colour
#            -- is the coordinate the shader receives the folded one?
#   all22    m405 --tfsall 1 --tfsdbg 22: a one-texture draw through a program
#            that samples nothing at all (MOV r0.rgb, color0) -- is the grey
#            the sample, or is the program not running?
#   m448dl   Goomba's Chip Flip's felt: the drawlog and the GL trace of the
#            frame the felt is black on
#   m432dl2  Dungeon Duos' walls: the same at +400
#
# One binary (~/MarioParty4-tfs.app).  Runs, each ~100 s:
#   probe    --tfsprobe: the extension, the builder's three programs compiled,
#            the read-back test (where the offset lands, the constant's grain)
#   m405 m417 m434   the warp games parked at entry +60/+400/+1200 (--tfslog)
#   m425     the Thwomps with the specular channel and the lit alpha
#   m425old  the same with --oldspec0 --nolitalpha (the M35 first-pass picture)
#   m448all  the felt through the shader (--tfsall 4): the Radeon's chain A/B
#
#   ~/m35/tfs/NAME.log, ~/m35/tfs/NAME/*.ppm, ~/m35/tfs/index.txt
cd "$HOME"
A="${TFS_APP:-$HOME/MarioParty4.app}/Contents/MacOS/isle"
D="${TFS_DIR:-$HOME/m37/tfs}"; mkdir -p "$D"
IDX="$D/index.txt"
RUNS=${*:-"p25 all22"}
echo "# tfs chain start $(date)  isle md5 $(md5 -q "$A")" >> "$IDX"
P="--com4 --rtc dolphin --freshcard --play board-start-com4.play --noconfig --lockstep --status --ovllog"

# a "port: fatal" puts up a dialog and hangs the chain (PLAN.md 50.2): kill by pid
( while true; do
    for l in "$D"/*.log; do
      if grep -q "port: fatal" "$l" 2>/dev/null; then
        n=$(basename "$l" .log)
        if ! grep -q "^$n EXIT" "$IDX" 2>/dev/null; then
          sleep 4; pid=$(ps ax | grep "[i]sle " | awk '{print $1}' | head -1); [ -n "$pid" ] && kill -9 $pid
        fi
      fi
    done
    sleep 10
    grep -q "tfs chain all done" "$IDX" 2>/dev/null && exit 0
  done ) &

run() {
    name=$1; g=$2; shift 2
    mkdir -p "$D/$name"
    t0=$(date +%s)
    "$A" --minigame "$g" --turns 1 $P --ffto 14000 --frames 20000 --mgdump 60,400,1200 --mgend 1260 \
        --shotdir "$D/$name" "$@" > "$D/$name.log" 2>&1
    e=$?
    echo "$name EXIT=$e $(( $(date +%s) - t0 ))s frames=$(ls "$D/$name"/*.ppm 2>/dev/null | wc -l | tr -d ' ') tfs='$(grep 'port> tfs (M35)' "$D/$name.log" | tail -1 | cut -c1-120)'" >> "$IDX"
    sleep 2; killall -9 isle 2>/dev/null; sleep 2
}

"$A" --tfsprobe --noconfig > "$D/probe.log" 2>&1
echo "probe EXIT=$? $(grep -c 'compiled' "$D/probe.log") compiled, $(grep -c REFUSED "$D/probe.log") refused" >> "$IDX"
sleep 2; killall -9 isle 2>/dev/null; sleep 2
for r in $RUNS; do
    case $r in
        m405) run m405 m405 --tfslog ;;
        m417) run m417 m417 --tfslog ;;
        m434) run m434 m434 --tfslog ;;
        m425) run m425 m425 ;;
        m425old) run m425old m425 --oldspec0 --nolitalpha ;;
        m448all) run m448all m448 --tfsall 4 --tfslog ;;
        m405all) run m405all m405 --tfsall 1 --tfslog --mgdump 400 --mgend 420 ;;
        m405all2) run m405all2 m405 --tfsall 2 --mgdump 400 --mgend 420 ;;
        m448) run m448 m448 ;;
        m432dl) run m432dl m432 --drawlog 2000 --drawlog-at 14877 ;;
        dbg*) run "$r" m405 --tfs --tfsdbg "${r#dbg}" --tfslog --mgdump 400 --mgend 420 ;;
        all*) run "$r" m405 --tfs --tfsall 1 --tfsdbg "${r#all}" --tfslog --mgdump 400 --mgend 420 ;;
        two*) run "$r" m405 --tfs --tfsall 2 --tfsdbg "${r#two}" --tfslog --mgdump 400 --mgend 420 ;;
        p*) run "$r" m434 --tfs --tfsdbg "${r#p}" --tfslog --mgdump 400 --mgend 420 ;;
        w*) run "$r" m417 --tfs --tfsdbg "${r#w}" --tfslog --mgdump 400 --mgend 420 ;;
        m434cpu) run m434cpu m434 --tfscopycpu --norenderthread --mgdump 400 --mgend 420 ;;
        m434sq) run m434sq m434 --tfssqcopy --mgdump 400 --mgend 420 ;;
        m434fb) run m434fb m434 --tfsforcebind --mgdump 400 --mgend 420 ;;
        m405fb) run m405fb m405 --tfsforcebind --mgdump 400 --mgend 420 ;;
        m405sq) run m405sq m405 --tfssqcopy --mgdump 400 --mgend 420 ;;
        m405cpu) run m405cpu m405 --tfscopycpu --norenderthread --mgdump 400 --mgend 420 ;;
        m405tex) run m405tex m405 --dumptex --mgdump 400 --mgend 420 ;;
        m405dump) run m405dump m405 --tfsdump 14870 --norenderthread --mgdump 400 --mgend 420 ;;
        m434dump) run m434dump m434 --tfsdump 14870 --tfslog --norenderthread --mgdump 400 --mgend 420 ;;
        m417dump) run m417dump m417 --tfsdump 14870 --tfslog --norenderthread --mgdump 400 --mgend 420 ;;
        m405dumpold) run m405dumpold m405 --tfsdump --tfsdbg 1 --norenderthread --mgdump 400 --mgend 420 ;;
        m403dl) run m403dl m403 --drawlog 2000 --drawlog-at 14877 ;;
        m448dl) run m448dl m448 --drawlog 3000 --drawlog-at 15343 --gltrace 15343 \
                    --tevstats --gxwarn --mgdump 1200 --mgend 1260 ;;
        m432dl2) run m432dl2 m432 --drawlog 3000 --drawlog-at 14877 --gltrace 14877 \
                    --tevstats --gxwarn --mgdump 400 --mgend 420 ;;
        m450dl) run m450dl m450 --drawlog 2000 --drawlog-at 14877 ;;
        *) run "$r" "${r%%-*}" ;;
    esac
done
echo "tfs chain all done $(date)" >> "$IDX"
