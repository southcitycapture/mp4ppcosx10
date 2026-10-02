#!/bin/sh
# session_video.sh -- a session recording as videos, end to end (M51, PLAN.md 66.5)
#
#   port/tools/session_video.sh REC [--from R] [--to R] [--name NAME] [--nodolphin]
#
#   ~/mp4-videos/NAME-g4.mp4     the G4 replaying REC in lockstep (every frame
#                                drawn, nothing dropped), every presented frame
#                                (--framedump: deflated on the G4, streamed here
#                                while it runs) with the mixer's own output
#                                (--wavdump), 59.94 frames a second
#   ~/mp4-videos/NAME-side.mp4   the G4 on the left, Dolphin on the right, in
#                                step (the console's field for each of the port's
#                                frames: tools/dolphin_sync.py's map), the G4's
#                                sound
#
# R are the recording's retraces (the game's frames from the boot): --from runs
# ahead undrawn to R (--ffto) and the videos start there; --to ends them (the
# recording's end by default).  The Dolphin side needs the sync's map
# (WORK/sync/map.json and session.dtm): made here by tools/dolphin_sync.py if
# it is not there yet (an hour or more for a board's worth of play).
#
# Runs on littlejelly: the G4 through the lab's job runner (~/bin/m52job and
# m52wait since M52 -- JOBRUN / JOBWAIT name others; one G4 job at a time),
# Dolphin through tools/dolphin_watch.py (one at a time).  APP: the G4 bundle
# in its home (default MarioParty4.app, the installed release).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
export PATH="$HOME/bin:$PATH"
REC=$1; shift
FROM=""; TO=""; NAME=""; DOLPHIN=1
while [ $# -gt 0 ]; do
  case $1 in
    --from) FROM=$2; shift 2 ;;
    --to) TO=$2; shift 2 ;;
    --name) NAME=$2; shift 2 ;;
    --nodolphin) DOLPHIN=0; shift ;;
    *) echo "session_video.sh: unknown $1" >&2; exit 2 ;;
  esac
done
[ -n "$NAME" ] || NAME=$(basename "$REC" .rec | tr ' ' '_')
END=$(sed -n 's/^end //p' "$REC" | tail -1)
[ -n "$TO" ] || TO=$END
[ -n "$FROM" ] || FROM=1
OUT=$HOME/mp4-videos
W=$OUT/work/$NAME
mkdir -p "$W"
FFMPEG=${FFMPEG:-$HOME/bin/ffmpeg}
APP=${APP:-${M51_APP:-MarioParty4.app}}
JOBRUN=${JOBRUN:-m52job}
JOBWAIT=${JOBWAIT:-m52wait}
say() { echo "$(date +%H:%M:%S) session_video $NAME: $*"; }

# ---- 1. the G4: the replay in lockstep, every frame dumped ----
say "the G4 replays retraces $FROM..$TO ($APP)"
ssh g4 "mkdir -p ~/m51/video && rm -f ~/m51/video/$NAME.fd ~/m51/video/$NAME.wav"
scp -q "$REC" "g4:m51/video/$NAME.rec"
FF=""
if [ "$FROM" -gt 120 ]; then FF="--ffto $((FROM - 60))"; fi
$JOBRUN "video-$NAME" <<EOF
\$HOME/$APP/Contents/MacOS/isle --replay \$HOME/m51/video/$NAME.rec --lockstep --nomenu --mute \
  --framedump \$HOME/m51/video/$NAME.fd --framedumpfrom $FROM --wavdump \$HOME/m51/video/$NAME.wav \
  $FF --frames $((TO + 2)) --status --log \$HOME/m51/video/$NAME.log
EOF
# the frames as they are written: a FIFO between the stream and the reader,
# kept lossless here (FFV1: about a tenth of the raw frames)
rm -f "$W/fd.fifo"; mkfifo "$W/fd.fifo"
( until ssh g4 "test -s ~/m51/video/$NAME.fd"; do sleep 10; done
  exec ssh g4 "tail -c +1 -f ~/m51/video/$NAME.fd" ) > "$W/fd.fifo" &
SPID=$!
python3 "$here/framedump_read.py" "$W/fd.fifo" --from "$FROM" --to "$TO" --index "$W/g4-index.txt" |
  "$FFMPEG" -y -v error -f rawvideo -pix_fmt rgb24 -s 640x480 -framerate 60000/1001 -i - -c:v ffv1 "$W/g4.mkv"
kill $SPID 2>/dev/null || true
rm -f "$W/fd.fifo"
# the job runner's own wait, repeated past its limit (a long replay)
until $JOBWAIT 3600 60 >/dev/null; do :; done
ssh g4 "rm -f ~/m51/video/$NAME.fd"
scp -q "g4:m51/video/$NAME.wav" "$W/g4.wav"
scp -q "g4:m51/video/$NAME.log" "$W/g4.log"
grep 'replay (M51)' "$W/g4.log" || true
# the audio from retrace FROM: the mixer's 32,000 samples a second from retrace 0
SS=$(python3 -c "print(f'{($FROM - 1) / (60000 / 1001):.6f}')")
DUR=$(python3 -c "print(f'{($TO - $FROM + 1) / (60000 / 1001):.6f}')")
"$FFMPEG" -y -v error -i "$W/g4.mkv" -ss "$SS" -t "$DUR" -i "$W/g4.wav" -map 0:v -map 1:a \
  -c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p -c:a aac -b:a 192k -shortest "$OUT/$NAME-g4.mp4"
say "wrote $OUT/$NAME-g4.mp4"
[ "$DOLPHIN" = 1 ] || exit 0

# ---- 2. Dolphin: the session's movie, every frame dumped ----
S=$W/sync
if [ ! -f "$S/session.dtm" ]; then
  say "no Dolphin map yet: tools/dolphin_sync.py (this takes a while)"
  python3 "$here/dolphin_sync.py" "$REC" "$S" --until "$TO"
fi
VEND=$(python3 -c "import json;m=json.load(open('$S/map.json'));print(m['vmap'][str($TO)]+60)")
python3 "$here/dolphin_watch.py" "$W/dolphin" --dtm "$S/session.dtm" --gecko "$S/GMPE01.ini" --dump \
  --audio "$W/dolphin.wav" --until-vc "$VEND" --timeout 7200 --watch 801D3CE0:ovl >/dev/null
# each Dolphin frame's field (its timestamp, fields since the boot)
python3 "$here/dolphin_fields.py" "$W/dolphin/frames.avi" > "$W/dolphin-fields.txt"

# ---- 3. side by side (piped: no raw file) ----
python3 "$here/side_by_side.py" --g4 "$W/g4.mkv" --g4-index "$W/g4-index.txt" --map "$S/map.json" \
  --dolphin "$W/dolphin/frames.avi" --dolphin-fields "$W/dolphin-fields.txt" --out - |
  "$FFMPEG" -y -v error -f rawvideo -pix_fmt rgb24 -s 1280x480 -framerate 60000/1001 -i - \
  -ss "$SS" -t "$DUR" -i "$W/g4.wav" -map 0:v -map 1:a -c:v libx264 -preset medium -crf 20 -pix_fmt yuv420p \
  -c:a aac -b:a 192k -shortest "$OUT/$NAME-side.mp4"
say "wrote $OUT/$NAME-side.mp4"
