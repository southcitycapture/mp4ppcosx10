/* A crash reporter, because the game runs on a stack the port allocated and a
 * debugger's unwinder cannot follow that.
 *
 * On SIGSEGV/SIGBUS the handler prints the faulting address and the program
 * counter, both raw and as an offset from the image's load address, so
 * `atos -o build-host/marioparty4 -l <base> <pc>` names the function.  It also
 * dumps the stub report, which is usually enough on its own: the last stub
 * called is nearly always next to the bug.
 */
#include "port.h"

#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(__APPLE__)
#include <dlfcn.h>

#include <dolphin/types.h>
#include <dolphin/vi.h>
#include <mach-o/dyld.h>
#define _XOPEN_SOURCE 700
#include <sys/ucontext.h>
#endif

static int watchdog_progressed(void);

static void handler(int sig, siginfo_t* info, void* uap) {
    if (sig == SIGALRM && watchdog_progressed()) {
        return; /* the game is moving; the alarm has re-armed */
    }
#if defined(__APPLE__)
    ucontext_t* uc = (ucontext_t*)uap;
    unsigned long long pc = 0, sp = 0;
    unsigned long long base = (unsigned long long)_dyld_get_image_header(0);
#if defined(__aarch64__)
    if (uc && uc->uc_mcontext) {
        pc = uc->uc_mcontext->__ss.__pc;
        sp = uc->uc_mcontext->__ss.__sp;
    }
#elif defined(__ppc__)
    if (uc && uc->uc_mcontext) {
        pc = uc->uc_mcontext->ss.srr0;
        sp = uc->uc_mcontext->ss.r1;
    }
#endif
    port_log("\n*** port: %s: signal %d at address %p\n",
             sig == SIGALRM ? "watchdog fired: no retrace in the last --watchdog period" : "fault",
             sig, info ? info->si_addr : NULL);
    /* Which region -- if any -- the address belongs to.  Since M8 the port
     * maps PROT_NONE guards either side of MEM1, the game stack and ARAM, so
     * an overrun faults one page past the end of the thing it overran instead
     * of 16 MB later wherever the address space happens to stop.  This is the
     * line that turns that into an answer (PLAN.md §18.1). */
    if (sig != SIGALRM && info && info->si_addr) {
        long off = 0;
        const void *rlo = NULL, *rhi = NULL, *glo = NULL, *ghi = NULL;
        const char* what = port_mem_region_name(info->si_addr, &off, &rlo, &rhi);
        const char* guards = port_mem_guard_of(info->si_addr, &glo, &ghi);
        if (what) {
            port_log("    region  %ld bytes into %s [%p, %p)\n", off, what, rlo, rhi);
        } else {
            port_log("    region  not a region the port owns\n");
        }
        if (guards) {
            port_log("            this is a guard page: %s ran off its %s end.\n",
                     guards, info->si_addr >= ghi ? "top" : "bottom");
            port_log("            %s is [%p, %p) -- the bug is the last write "
                     "before this one.\n", guards, glo, ghi);
        }
    }
    port_log("    pc   %016llx  (image base %016llx, offset %llx)\n", pc, base,
             pc > base ? pc - base : 0);
    port_log("    sp   %016llx\n", sp);
    port_log("    atos -o <binary> -l 0x%llx 0x%llx\n", base, pc);
    {
        Dl_info di;
        if (pc && dladdr((void*)(uintptr_t)pc, &di) && di.dli_sname) {
            port_log("    in   %s  (%s)\n", di.dli_sname,
                     di.dli_fname ? di.dli_fname : "?");
        } else if (pc) {
            port_log("    in   no image claims this pc -- a REL bundle that has "
                     "been dlclose'd, or a wild jump\n");
        }
    }
    /* A backtrace, walked by hand.  No unwinder can follow this stack: the
     * game runs on one the port allocated (port_call_on_stack) and its
     * HUPROCESS coroutines swap `sp` under everyone's feet with the
     * hand-written gcsetjmp in src/game/jmp.c.  But the PowerPC linkage
     * convention is simple enough to walk without one -- the word at r1 is
     * the caller's frame and the return address is eight bytes into it -- and
     * naming the frames is the difference between "signal 11 somewhere" and
     * an answer.  Bounded, and every dereference is range-checked, because
     * this runs inside a fault handler and must not fault again. */
#if defined(__ppc__)
    if (sp) {
        unsigned long frame = (unsigned long)sp;
        int depth;
        port_log("    backtrace (pc, then return addresses up the linkage chain):\n");
        for (depth = 0; depth < 24; depth++) {
            unsigned long next, lr;
            Dl_info di;
            if (frame < 0x1000 || (frame & 3) != 0) {
                break;
            }
            next = *(const unsigned long*)frame;
            if (next <= frame || next < 0x1000 || (next & 3) != 0) {
                break;
            }
            lr = *(const unsigned long*)(next + 8);
            if (!lr) {
                break;
            }
            if (dladdr((void*)lr, &di) && di.dli_sname) {
                const char* f = di.dli_fname ? di.dli_fname : "?";
                const char* slash = f;
                const char* q;
                for (q = f; *q; q++) {
                    if (*q == '/') {
                        slash = q + 1;
                    }
                }
                port_log("      #%-2d %08lx  %s + %lu  [%s]\n", depth, lr, di.dli_sname,
                         (unsigned long)((char*)lr - (char*)di.dli_saddr), slash);
            } else {
                port_log("      #%-2d %08lx  (no image -- unloaded REL?)\n", depth, lr);
            }
            frame = next;
        }
    }
#endif
#else
    (void)uap;
    port_log("\n*** port: signal %d at address %p\n", sig, info ? info->si_addr : NULL);
#endif
    port_stub_report();
    /* The whole point of the ring: a fault is where a snapshot stops being a
     * curiosity and becomes the shortest way back to this moment. */
    port_snap_report_existing();
    _exit(128 + sig);
}

/* --watchdog N: a hang is the most likely M1 failure, and a hung game gives a
 * debugger nothing to unwind because it runs on the port's own stack.  SIGALRM
 * lands in the same handler, which prints the program counter to look up with
 * atos and the stub table, whose last entry is nearly always next to the
 * problem. */
/* --watchdog N is a *progress* watchdog, not a stopwatch.  It used to be a
 * plain `alarm(N)` that fired after N seconds whether or not the game was
 * healthy, and then reported "the game is not making progress" -- which, when
 * the game was simply slower than N seconds, is a lie that costs whoever reads
 * it an hour.  It now re-arms every period and only reports when the retrace
 * count has not moved since the previous one, which is the question actually
 * being asked. */
static int wd_period;
static unsigned wd_last_retrace;
static int wd_seen;

/* Returns 1 if the alarm should be treated as a hang, 0 if it re-armed. */
static int watchdog_progressed(void) {
    unsigned now = VIGetRetraceCount();
    if (wd_seen && now == wd_last_retrace) {
        return 0; /* stuck */
    }
    wd_seen = 1;
    wd_last_retrace = now;
    alarm((unsigned)wd_period);
    return 1;
}

void port_watchdog_arm(int seconds) {
    wd_period = seconds;
    wd_seen = 0;
    alarm((unsigned)seconds);
}

void port_crash_handler_install(void) {
    struct sigaction sa;
    /* PORT_NO_CRASH_HANDLER=1 leaves the faults to a debugger, which is the
     * only way to get a stack out of the game's own coroutine stacks. */
    if (getenv("PORT_NO_CRASH_HANDLER")) {
        return;
    }
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL);
}
