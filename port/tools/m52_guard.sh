#!/bin/sh
# M52 (PLAN.md 67): the timed run's guard, ON THE G4.  Before (and after) a
# run at real time: is the display awake, and is nothing else busy (Spotlight's
# mds/mdworker, a periodic job)?  Changes no setting: a sleeping display is
# woken the way any game keeps it awake -- UpdateSystemActivity(UsrActivity),
# what SDL's event pump already calls every 30 s while the game runs -- and a
# busy machine is waited for (at most GUARD_WAIT, 600 s).
#
#   m52_guard.sh snap FILE / delta FILE -> "during: others N s of CPU ... ok|BAD"
#   m52_guard.sh [pre|post]  -> one line: "display=4 others=1.3% top=NAME:N% ok"
#                               (exit 0) or "... BAD" (exit 1)
# CurrentPowerState 4 = the display on (IODisplayWrangler); others = the %CPU
# of every process but the game, this script's own and WindowServer (whose
# CPU after a run is the game's own window being composited: the first
# scoreboard's post readings had it at 16-25%) (ps's decaying average).
disp() { ioreg -n IODisplayWrangler -r -d 1 | sed -n 's/.*"CurrentPowerState"=\([0-9]*\).*/\1/p' | head -1; }
wake() { python -c 'import ctypes; ctypes.CDLL("/System/Library/Frameworks/CoreServices.framework/CoreServices").UpdateSystemActivity(1)' 2>/dev/null; }
others() {
    ps -axo %cpu,command | awk 'NR > 1 && $2 !~ /MacOS\/isle$/ && $0 !~ /MacOS\/isle / && $2 !~ /^(ps|awk|sh|sed|sort|head)$/ && $2 !~ /WindowServer$/ {
        s += $1; if ($1 > tm) { tm = $1; tn = $2 } }
        END { n = split(tn, a, "/"); printf "%.1f %s:%.1f\n", s, a[n], tm }'
}
# snap FILE / delta FILE: every process's CPU time (ps cputime) before a timed
# run and the difference after it -- what each other process really used
# during the run, read outside the run (a sampler during it cost the edge
# screens their frames: ps every 5 s took m409 from 29.1 to 28.6).  The
# scoreboard's first count lost a Toad's Quick Draw run to 12 s at 13-19 fps
# that neither the pre nor the post reading saw: Finder's network browsing
# (smbclient, nmblookup, DirectoryService), seen in other runs' readings.
# BAD when the others used over GUARD_CPU (3.0) s.
cpu() { ps -axo pid,cputime,command | awk 'NR > 1 && $3 !~ /MacOS\/isle$/ && $0 !~ /MacOS\/isle / && $3 !~ /WindowServer$/ && $3 !~ /^(ps|awk|sh|sed|sort|head)$/ {
    n = split($2, t, ":"); s = 0; for (i = 1; i <= n; i++) s = s * 60 + t[i]
    m = split($3, a, "/"); print $1, s, a[m] }'; }
if [ "${1:-}" = snap ]; then
    cpu > "$2"; exit 0
fi
if [ "${1:-}" = delta ]; then
    cpu > "$2.after"
    awk -v lim="${GUARD_CPU:-3.0}" 'NR == FNR { b[$1] = $2; next }
        ($1 in b) { d = $2 - b[$1]; if (d > 0) { tot += d; if (d > mx) { mx = d; mn = $3 } } }
        END { printf "during: others %.1f s of CPU (top %s %.1f s) %s\n", tot, mn ? mn : "-", mx, (tot > lim) ? "BAD" : "ok" }' "$2" "$2.after"
    exit 0
fi
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
