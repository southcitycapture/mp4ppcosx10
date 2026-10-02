#!/bin/sh
# M52 (PLAN.md 67): the RC1 flow's dry run, ON THE G4 (a job of the lab's
# runner: tools/m52_job.sh).  The user's marathon as Developer Mode starts it
# -- `isle --marathon PLAN` -- over MT_LIST (ten minigames in a row), one
# person (a scripted stand-in for player 1: ref/movies/mg-human-m52.play, the plan's
# childflags) and three COMs, every minigame recorded; after MT_STOPAT
# results the child then playing is stopped the way "Stop the marathon here"
# stops it (the plan's .stop file, the child closed -- by its pid), the
# summary is shot, and the marathon is resumed (`isle --marathon PLAN` again,
# what "Resume the stopped marathon" runs) to its end; the summary is shot
# again.  The lab's config is saved and put back; the player's card is never
# touched (the children play on the scratch card).
#
#   MT_APP (bundle)  MT_LIST (401,...)  MT_STOPAT (3)  MT_DIR (~/m52/mt)
APP=${MT_APP:-$HOME/MarioParty4.app}/Contents/MacOS/isle
D=${MT_DIR:-$HOME/m52/mt}
AS="$HOME/Library/Application Support/MarioParty4"
REC="$HOME/Documents/MarioParty4 Recordings"
mkdir -p "$D"
cp "$AS/config" "$D/config.saved" 2>/dev/null
ls "$REC" > "$D/recordings-before.txt" 2>/dev/null
PLAN="$D/plan.txt"
cat > "$PLAN" <<P
# Mario Party 4 PowerPC Edition -- a minigame marathon (Developer Mode, M52 dry run)
list = ${MT_LIST:-409,410,411,412,413,414,415,416,417,418}
humans = 1
cast = mario,luigi,peach,yoshi
record = 1
next = 0
childflags = --keepplay --play $HOME/m52/mg-human-m52.play
P
cp "$PLAN" "$D/plan-start.txt"
log() { echo "$(date +%H:%M:%S) $*" >> "$D/dry.txt"; }
results() { grep -c '^r[0-9]* = ' "$PLAN"; }
summary_shot() {
    # the game the driver starts again shows the summary: a picture, then it goes (by pid)
    k=0
    while [ $k -lt 120 ]; do
        p=$(ps -axo pid,command | grep -v grep | grep "MacOS/isle --marathonresult" | awk '{print $1}' | head -1)
        [ -n "$p" ] && break
        sleep 2; k=$((k + 1))
    done
    [ -n "$p" ] || { log "no summary game"; return; }
    sleep 25
    screencapture -x "$D/$1.png"
    log "summary shot $1.png (pid $p)"
    kill $p; sleep 3; kill -9 $p 2>/dev/null
}

log "start: $(md5 -q "$APP") list ${MT_LIST:-409..418}"
"$APP" --marathon "$PLAN" --log "$D/driver1.log" > /dev/null 2>&1 &
DPID=$!
# the stop: after MT_STOPAT results, the next child is stopped mid-minigame
# as the overlay's "Stop the marathon here" does (the .stop file, the child closed)
while kill -0 $DPID 2>/dev/null && [ "$(results)" -lt "${MT_STOPAT:-3}" ]; do sleep 10; done
k=0
while [ $k -lt 60 ]; do
    c=$(ps -axo pid,command | grep -v grep | grep "MacOS/isle --minigame" | awk '{print $1}' | head -1)
    [ -n "$c" ] && break
    sleep 2; k=$((k + 1))
done
sleep 120   # well into the next minigame (the hurry is ~60 s)
echo stop > "$PLAN.stop"
log "stop: child pid $c after $(results) results"
kill $c
while kill -0 $DPID 2>/dev/null; do sleep 5; done
cp "$PLAN" "$D/plan-stopped.txt"
summary_shot summary-stopped
cp "$AS/marathon-result.txt" "$D/summary-stopped.txt" 2>/dev/null
# the resume
log "resume from next = $(sed -n 's/^next = //p' "$PLAN")"
"$APP" --marathon "$PLAN" --log "$D/driver2.log" > /dev/null 2>&1 &
DPID=$!
while kill -0 $DPID 2>/dev/null; do sleep 10; done
cp "$PLAN" "$D/plan-end.txt"
summary_shot summary-end
cp "$AS/marathon-result.txt" "$D/summary-end.txt" 2>/dev/null
cp -R "$AS/marathon-logs" "$D/" 2>/dev/null
ls -l "$REC" > "$D/recordings-after.txt"
[ -f "$D/config.saved" ] && cp "$D/config.saved" "$AS/config"
log "done: $(results) results"
