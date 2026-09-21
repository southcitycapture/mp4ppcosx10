# gdb -batch -x m458.gdb ~/isle.app/Contents/MacOS/isle PID   (M30, PLAN.md 45)
# Attach to a --restore of m458's pre-fault snapshot, wait for the SIGBUS in
# m458Dll.bundle, and print what the rules of g4-witness.md 0b allow: the
# registers, the code at the pc, the backtrace, the REL's load address (so the
# pc can be resolved against the bundle's disassembly offline).  No `info
# symbol`, no inferior calls, nothing that can error before `detach`.
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
info registers pc lr ctr r0 r1 r2 r3 r4 r5 r6 r7 r8 r9 r10 r11 r12 r13 r14 r15 r16 r17 r18 r19 r20 r21 r22 r23 r24 r25 r26 r27 r28 r29 r30 r31
printf "=== code at pc\n"
x/12i $pc-24
printf "=== frame_no\n"
print 'gl13.c'::frame_no
printf "=== bt\n"
bt 40
printf "=== sharedlibrary\n"
info sharedlibrary
printf "=== line\n"
info line *$pc
printf "=== done; detaching\n"
detach
quit
