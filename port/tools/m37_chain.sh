#!/bin/sh
# M37 (PLAN.md 52): the character select's batch enders, A/B'd ON THE G4 as
# the console runner's job (`g4 use MarioParty4.app; g4 run [RUNS...]`), the
# M33/M36 chain's shape.
#
# One binary (~/MarioParty4.app).  The classes are bits of --cmpmask, the
# bisect lever every compare-first setter has had since M18 (PLAN.md 33.2):
#
#   255   the M36 shape: neither new class
#   511   + the compare-first setters of M37 (bit 256)
#   767   + the descriptor setters that end no batch (bit 512)
#   1023  both (the M37 default)
#
# Runs:
#   E        the measurement: a turbo walk with --endlog at 800/3000/7000 and
#            --gltrace at the character select's frame
#   E0       the same on the M36 shape (--cmpmask 255)
#   T<mask>  --turbo 9,000 frames, the three md5s
#   R<mask>  --realtime 16,000 frames, presented fps (m33_perfstat.py)
#            a trailing letter is a repeat: R1023a, R1023b
#
#   ~/m37/NAME.log, ~/m37/NAME/{frame-*.ppm,perf.csv}, ~/m37/md5s.txt,
#   ~/m37/index.txt (one line per run: exit code, wall seconds, the md5s)
cd "$HOME"
D="${M37_DIR:-$HOME/m37}"; mkdir -p "$D"
IDX="$D/index.txt"
COMMON="--com4 --rtc dolphin --freshcard --noconfig --status --ovllog --perf \
--dumpframe 800,3000,7000 --perfwin 700-870:title,2600-3600:charselect,6000-8900:board"
TURBO="--turbo --play board-start-com4.play --frames 9000"
REAL="--soak --realtime --frames 16000"
MEAS="--turbo --play board-start-com4.play --frames 7100 --submitstats \
--endlog 800,3000,7000 --gltrace 3000"
echo "# m37 chain start $(date)  isle md5 $(md5 -q "$HOME/MarioParty4.app/Contents/MacOS/isle")" >> "$IDX"

run() {
    name=$1; app=$2; ceiling=$3; shift 3
    mkdir -p "$D/$name"
    rm -f "$D/$name"/*.ppm
    echo "chain: $name start $(date)"
    t0=$(date +%s)
    "$HOME/$app/Contents/MacOS/isle" $COMMON --shotdir "$D/$name" --perfdump "$D/$name/perf.csv" "$@" > "$D/$name.log" 2>&1 &
    pid=$!
    while kill -0 $pid 2>/dev/null; do
        sleep 5
        if [ $(( $(date +%s) - t0 )) -gt "$ceiling" ]; then
            echo "chain: $name over the $ceiling s ceiling -- killed" | tee -a "$D/$name.log"
            kill -9 $pid 2>/dev/null
        fi
    done
    wait $pid; e=$?
    t1=$(date +%s)
    m=""
    for f in 800 3000 7000; do
        p="$D/$name/frame-0$f.ppm"
        [ $f -lt 1000 ] && p="$D/$name/frame-00$f.ppm"
        if [ -f "$p" ]; then
            h=$(md5 -q "$p" | cut -c1-8)
        else
            h="--------"
        fi
        m="$m $f:$h"
        echo "$name $f $h" >> "$D/md5s.txt"
    done
    echo "$name EXIT=$e wall=$((t1 - t0))s md5$m args='$*'" >> "$IDX"
    echo "chain: $name done EXIT=$e $(date)"
    sleep 2; killall -9 isle 2>/dev/null; sleep 3
}

# the mask out of the run name: R1023b -> 1023, E -> the build's default
mask_of() { echo "$1" | sed 's/^[A-Z]//; s/[a-z]*$//'; }

[ $# -gt 0 ] && M37_RUNS="$*"
for r in ${M37_RUNS:-E}; do
    m=$(mask_of "$r")
    case "$m" in ''|*[!0-9]*) f="" ;; *) f="--cmpmask $m" ;; esac
    case $r in
        E*)  run $r MarioParty4.app 900 $MEAS $f ;;
        T*)  run $r MarioParty4.app 700 $TURBO $f ;;
        R*)  run $r MarioParty4.app 480 $REAL $f ;;
        *)   echo "chain: unknown run $r" ;;
    esac
done
echo "# m37 chain all done $(date)" >> "$IDX"
echo "chain: all done $(date)"
