set unwindonsignal on
# The safe Hu3DData walk: every model with a live motion, no stack reads and no
# symbol lookups, so it cannot error and therefore cannot kill the inferior
# (PLAN.md 23.1).  Run with port/tools/gdb/mpgdb on the G4.
set print pretty off
printf "GlobalCounter=%u Hu3DPauseF=%d minimumVcountf=%f\n", GlobalCounter, Hu3DPauseF, minimumVcountf
set $i = 0
while $i < 512
  set $m = &Hu3DData[$i]
  if $m->hsf != 0 && $m->motId != -1
    printf "m%03d motId=%d attr=%08x motAttr=%08x t=%f sp=%f st=%f en=%f layer=%d cam=%04x shift=%d tick=%d\n", $i, $m->motId, $m->attr, $m->motAttr, $m->motWork.time, $m->motWork.speed, $m->motWork.start, $m->motWork.end, $m->layerNo, $m->cameraBit, $m->motIdShift, $m->tick
  end
  set $i = $i + 1
end
detach
quit
