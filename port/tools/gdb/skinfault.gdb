# gdb -batch -x skinfault.gdb ~/isle.app/Contents/MacOS/isle PID   (M19)
# Attach to a --restore of skinfault-f050000.snap, wait for the fault in
# gx_skin_array_bound, and print the registry entry it was reading, whether
# its HSF's heap block is still allocated, and which live model (if any)
# still owns that HSF.  Rules of g4-witness.md 0b: unwindonsignal first,
# no `info symbol`, no inferior calls, nothing that can error before the
# state is printed.
set unwindonsignal on
set print pretty off
handle SIGSEGV stop print nopass
handle SIGBUS stop print nopass
handle SIGALRM nostop noprint pass
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
handle SIGCHLD nostop noprint pass
handle SIGPIPE nostop noprint pass
handle SIGTERM nostop noprint pass
continue
printf "=== stopped\n"
info registers pc lr r3 r4 r5 r6 r7 r8 r9 r10 r11 r12 r27 r28 r29 r30 r31
printf "=== frame_no\n"
print 'gl13.c'::frame_no
printf "=== bt\n"
bt 30
printf "=== frame 0\n"
frame 0
info locals
print p
print m
print *m
print *m->owner
printf "=== hsf heap block header (size magic/flag prev next num retaddr) at hsf-32\n"
x/8wx ((char*)m->owner->hsf - 32)
printf "=== the block containing p, header search is not possible: p itself\n"
x/8wx p
printf "=== obj pointer (raw), and the mesh.vertex field it holds now\n"
print m->obj
x/16wx m->obj
printf "=== live models using this hsf\n"
set $i = 0
set $n = 0
while $i < 512
  if Hu3DData[$i].hsf == m->owner->hsf
    printf "Hu3DData[%d].hsf == stale hsf (attr %x)\n", $i, Hu3DData[$i].attr
    set $n = $n + 1
  end
  set $i = $i + 1
end
printf "live models with this hsf: %d\n", $n
printf "=== registry: every entry\n"
set $j = 0
while $j < 'gx_skin.c'::nhsfs
  printf "hsfs[%d] hsf=%p sig=%08x serial=%u mtx_dirty=%d skin_dirty=%d cpu=%d last_frame=%u nmesh=%d\n", $j, 'gx_skin.c'::hsfs[$j].hsf, 'gx_skin.c'::hsfs[$j].signature, 'gx_skin.c'::hsfs[$j].serial, 'gx_skin.c'::hsfs[$j].mtx_dirty, 'gx_skin.c'::hsfs[$j].skin_dirty, 'gx_skin.c'::hsfs[$j].cpu, 'gx_skin.c'::hsfs[$j].last_frame, 'gx_skin.c'::hsfs[$j].nmesh
  set $j = $j + 1
end
printf "=== up the stack: the model being drawn\n"
up
up
up
up
info frame
info locals
printf "=== done; detaching (the port's own handler then reports and exits)\n"
detach
quit
