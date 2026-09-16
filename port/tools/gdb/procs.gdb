# gdb -batch -x port/tools/gdb/procs.gdb ~/isle.app/Contents/MacOS/isle PID
# Walk the HUPROCESS list and say where each coroutine is parked: the
# resume point (always HuPrcVSleep) and its caller, read off the saved
# stack.  jmp_buf = {lr, cr, sp, r2, ...} (port/src/os/jmp_ppc_darwin.s).
#
# RULES (2026-09-16, this killed a 7-hour soak once):
#  * set unwindonsignal on BEFORE anything else.  Apple gdb-768 answers
#    `info symbol` / `print` on an unmapped address by calling
#    objc_lookUpClass *inside the game*; a fault there is a fault in the
#    game, and -batch then detaches from a dead process.
#  * never `info symbol`; use `info line *ADDR` (no inferior call).
#  * skip processes whose stat has bit 2/3 set (exiting/dead): their saved
#    stack is stale.
set unwindonsignal on
set print pretty off
set $p = processtop
set $i = 0
while $p != 0 && $i < 24
  printf "proc %d @%p prio=%d stat=%d exec=%d sleep=%d\n", $i, $p, $p->prio, $p->stat, $p->exec, $p->sleep_time
  if ($p->stat & 12) == 0
    set $sp = $p->jump.sp
    if $sp > 0x1000 && $sp < 0x40000000
      set $c1 = *(unsigned int*)$sp
      if $c1 > 0x1000 && $c1 < 0x40000000
        set $r1 = *(unsigned int*)($c1 + 8)
        printf "   parked in caller: "
        info line *$r1
        set $c2 = *(unsigned int*)$c1
        if $c2 > 0x1000 && $c2 < 0x40000000
          set $r2 = *(unsigned int*)($c2 + 8)
          printf "   and its caller:   "
          info line *$r2
        end
      end
    end
  end
  set $p = $p->next
  set $i = $i + 1
end
detach
quit
