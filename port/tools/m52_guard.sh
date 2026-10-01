#!/bin/sh
# M52 (PLAN.md 67): the timed run's guard, ON THE G4.  Before (and after) a
# run at real time: is the display awake, and is nothing else busy (Spotlight's
# mds/mdworker, a periodic job)?  Changes no setting: a sleeping display is
# woken the way any game keeps it awake -- UpdateSystemActivity(UsrActivity),
# what SDL's event pump already calls every 30 s while the game runs -- and a
# busy machine is waited for (at most GUARD_WAIT, 600 s).
#
#   m52_guard.sh [pre|post]  -> one line: "display=4 others=1.3% top=NAME:N% ok"
#                               (exit 0) or "... BAD" (exit 1)
# CurrentPowerState 4 = the display on (IODisplayWrangler); others = the %CPU
# of every process but the game and this script's own (ps's decaying average).
disp() { ioreg -n IODisplayWrangler -r -d 1 | sed -n 's/.*"CurrentPowerState"=\([0-9]*\).*/\1/p' | head -1; }
wake() { python -c 'import ctypes; ctypes.CDLL("/System/Library/Frameworks/CoreServices.framework/CoreServices").UpdateSystemActivity(1)' 2>/dev/null; }
others() {
    ps -axo %cpu,command | awk 'NR > 1 && $2 !~ /MacOS\/isle$/ && $0 !~ /MacOS\/isle / && $2 !~ /^(ps|awk|sh|sed|sort|head)$/ {
        s += $1; if ($1 > tm) { tm = $1; tn = $2 } }
        END { n = split(tn, a, "/"); printf "%.1f %s:%.1f\n", s, a[n], tm }'
}
mode=${1:-pre}; t=0; lim=${GUARD_WAIT:-600}
while :; do
    d=$(disp); [ "$d" = 4 ] || { wake; sleep 3; d=$(disp); }
    set -- $(others); o=$1; top=$2
    ok=$(awk -v o="$o" -v d="$d" 'BEGIN { print (d == 4 && o < 15.0) ? 1 : 0 }')
    if [ "$ok" = 1 ] || [ "$mode" = post ] || [ $t -ge $lim ]; then break; fi
    sleep 10; t=$((t + 10))
done
if [ "$ok" = 1 ]; then echo "display=$d others=$o% top=$top waited=${t}s ok"; exit 0; fi
echo "display=$d others=$o% top=$top waited=${t}s BAD"; exit 1
