#!/bin/sh
# M52 (PLAN.md 67; M51's m51_job.sh in ~/m52): the G4's job runner -- installed as the chain app's
# executable (~/MarioParty4-chain.app/Contents/MacOS/isle) so the console
# runner starts it in the console session (a GL context, the pads): it runs
# ~/m52/job.sh (written by the lab for each job), then appends
# "DONE <name> <exit> <seconds>" to ~/m52/index.txt.
cd "$HOME"
mkdir -p "$HOME/m52"
J="$HOME/m52/job.sh"
name=$(sed -n 's/^# name: //p' "$J" | head -1)
t0=$(date +%s)
echo "START ${name:-job} $(date)" >> "$HOME/m52/index.txt"
sh "$J" > "$HOME/m52/job.out" 2>&1
e=$?
echo "DONE ${name:-job} exit=$e wall=$(( $(date +%s) - t0 ))s $(date)" >> "$HOME/m52/index.txt"
