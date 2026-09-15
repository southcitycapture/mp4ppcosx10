#!/bin/sh
# audio_ab.sh -- the controlled A/B the audio budget has never had.
#
# PLAN.md §17.6 compared a 4-tap figure taken from 119 seconds of boot and
# menus against a linear figure taken from somewhere else, called it 1.53 ms
# against a 1.5 ms budget, and §17.10 corrected it to 2.21 ms over 36 minutes
# of real gameplay.  Both numbers were honestly measured and neither was an
# experiment, because the two runs were not the same run.
#
# §17.9 item 3 says what the experiment is: two runs of the same command
# differing only by --resample1, on the same segment, with --rtc and
# --freshcard making the run a function of its command line (§17.2).  This is
# that, as one command, so that it is cheap enough to do every time rather
# than once when someone remembers.
#
# Usage, on the Mac, against the G4:
#
#   port/tools/audio_ab.sh --frames 20000 --minigame m425
#   G4=1 port/tools/audio_ab.sh --frames 40000            # boot + board
#
# Any extra arguments are passed to both runs unchanged.  The pinned part of
# the command line -- the part that makes it an experiment -- is not
# overridable: --rtc dolphin, a fresh card in a scratch file, four CPU players,
# --turbo, --perf and --clickstat.
#
# What to read: `aud` is the per-frame cost of the mixer against a 1.5 ms
# budget, and mean is the number the budget is about; worst is the one stutter
# is about.  --clickstat is §16.7's discontinuity count, and it is the reason
# the 4-tap filter exists -- if linear is cheaper *and* clicks no more, the
# 4-tap has not bought anything and linear should be the default.
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)

: "${IMAGE_PATH:=$HOME/PowerPC Project/ROMs/Mario Party 4 (USA) (Rev 1).nkit.iso}"
: "${OUT:=/tmp/mp4-audio-ab}"
mkdir -p "$OUT"

COMMON="--rtc dolphin --freshcard --com4 --turbo --perf --clickstat --headless --status"

: "${G4_WAIT_MAX:=5400}" # seconds to wait for one G4 run to finish

run() {
    name=$1
    shift
    if [ -n "$G4" ]; then
        g4=$repo/../isle-ppc-tools/g4/g4
        # `g4 run` hands the request to the console runner and returns in about
        # four seconds -- it does NOT wait for the game to exit.  Launching both
        # sides of an A/B through it without waiting runs the second `killall
        # isle` over the first, and leaves two logs holding the twelve lines
        # `g4 run` happened to tail.  So: launch, then poll the remote log for
        # the runner's own EXITCODE line, then pull the whole thing.  --frames
        # is what makes the run end by itself; without it this waits out
        # G4_WAIT_MAX and says so.
        "$g4" run $COMMON --card "\$HOME/ab-$name.raw" "$@" >/dev/null 2>&1 || true
        waited=0
        while [ "$waited" -lt "$G4_WAIT_MAX" ]; do
            if "$g4" ssh 'grep -q EXITCODE "$HOME/isle-log.txt" 2>/dev/null' >/dev/null 2>&1; then
                break
            fi
            sleep 15
            waited=$((waited + 15))
        done
        [ "$waited" -lt "$G4_WAIT_MAX" ] || echo "  (warning: $name did not finish in ${G4_WAIT_MAX}s)"
        "$g4" ssh 'cat "$HOME/isle-log.txt"' > "$OUT/$name.log" 2>&1 || true
    else
        "$repo/port/build-host/marioparty4" --image "$IMAGE_PATH" \
            $COMMON --card "$OUT/$name.raw" "$@" > "$OUT/$name.log" 2>&1 || true
    fi
}

echo "A: 4-tap Catmull-Rom (--resample4)"
run 4tap --resample4 "$@"
echo "B: linear (the default since PLAN.md 20.5)"
run linear --resample1 "$@"

echo
printf '%-8s %-56s %s\n' "run" "aud (ms)" "clicks"
for n in 4tap linear; do
    aud=$(grep -E '^  aud ' "$OUT/$n.log" | tail -1 | sed 's/^  aud *//')
    clicks=$(grep -o 'clickstat: [0-9]* step' "$OUT/$n.log" | tail -1 | tr -d 'a-z:' )
    frames=$(grep -c 'status f' "$OUT/$n.log" || true)
    printf '%-8s %-56s %s\n' "$n" "${aud:-(no aud line -- the run did not finish)}" "${clicks:-?}"
done
echo
echo "logs: $OUT/4tap.log  $OUT/linear.log"
echo
echo "The run is only an experiment if both sides reached the same place."
echo "Check that with the last status line of each:"
for n in 4tap linear; do
    printf '  %-7s %s\n' "$n" "$(grep 'status f' "$OUT/$n.log" | tail -1 | sed 's/^port> //')"
done
