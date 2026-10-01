#!/bin/sh
# m51_soakplan.sh PLAN -- a soak plan written by Developer Mode's soak planner
# (src/ui/soakplan.c; ~/Library/Application Support/MarioParty4/soak-plan.txt)
# as the game's flags, for the lab's chain tools (M51, PLAN.md 66.1):
#
#   isle $(sh port/tools/m51_soakplan.sh soak-plan.txt) --log ~/isle-log.txt
#
# The same expansion `isle --soakplan PLAN` makes inside the game.
P=${1:?usage: m51_soakplan.sh PLAN}
get() { sed -n "s/^ *$1 *= *\([^#]*\).*/\1/p" "$P" | tail -1 | sed 's/ *$//'; }
a="--soak --com4 --rtc dolphin --freshcard --status --perf --stuckwatch 200 --ovllog"
v=$(get boards);    [ -n "$v" ] && a="$a --board $v"
v=$(get turns);     [ -n "$v" ] && [ "$v" -gt 0 ] && a="$a --turns $v"
v=$(get minigames); [ -n "$v" ] && [ "$v" != all ] && a="$a --minigame $v"
v=$(get lite);      case $v in on) a="$a --lite" ;; off) a="$a --nolite" ;; auto) a="$a --liteauto" ;; esac
v=$(get liteopts);  [ -n "$v" ] && a="$a --liteopts $v"
v=$(get water);     [ -n "$v" ] && a="$a --water $v"
v=$(get snapshots); [ -n "$v" ] && [ "$v" -gt 0 ] && a="$a --snap-every $v --snap-keep 3"
v=$(get minutes);   [ -n "$v" ] && [ "$v" -gt 0 ] && a="$a --frames $((v * 3600))"
v=$(get flags);     [ -n "$v" ] && a="$a $v"
echo "$a"
