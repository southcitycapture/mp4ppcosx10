#!/bin/sh
# Take N `sample` profiles of the running port on the G4, each tagged with the
# scene that `--ovllog` last named.
#
#   (on the G4)   ~/g4_sampler.sh OUTDIR [N] [SECONDS] [GAP]
#
# Why the tag rather than a stopwatch: a profile is only worth anything if you
# know which screen it is a profile *of*, and the walk does not reach the
# character select or the board at a predictable wall-clock time -- it reaches
# them when it reaches them, and that moves every time the port's speed moves,
# which is the very thing being measured.  Run the port with `--ovllog` and
# each sample file is named for the overlay that was live when it started and
# when it finished; a file whose two halves disagree straddles a transition and
# should be thrown away.
#
# Tiger/Leopard: `sample` is in /usr/bin and works over SSH.  The process is
# named `isle` for the reason port/tools/make_bundle.sh explains.
out=$1; n=${2:-8}; secs=${3:-10}; gap=${4:-15}
[ -n "$out" ] || { echo "usage: g4_sampler.sh OUTDIR [N] [SECONDS] [GAP]" >&2; exit 2; }
mkdir -p "$out"; rm -f "$out"/*.txt
i=1
while [ $i -le $n ]; do
  before=$(grep "overlay" ~/isle-log.txt | tail -1 | sed 's/.*frame \([0-9]*\).*overlay \(-*[0-9]*\).*/f\1-ovl\2/')
  sample isle $secs -file "$out/s$i.raw" >/dev/null 2>&1
  after=$(grep "overlay" ~/isle-log.txt | tail -1 | sed 's/.*frame \([0-9]*\).*overlay \(-*[0-9]*\).*/f\1-ovl\2/')
  [ -f "$out/s$i.raw" ] && mv "$out/s$i.raw" "$out/s$i.${before:-x}.to.${after:-x}.txt"
  i=$((i+1))
  sleep $gap
done
echo "g4_sampler: $n profiles in $out"
