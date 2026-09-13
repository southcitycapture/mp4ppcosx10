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
#include <mach-o/dyld.h>
#define _XOPEN_SOURCE 700
#include <sys/ucontext.h>
#endif

static void handler(int sig, siginfo_t* info, void* uap) {
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
             sig == SIGALRM ? "watchdog fired (the game is not making progress)" : "fault",
             sig, info ? info->si_addr : NULL);
    port_log("    pc   %016llx  (image base %016llx, offset %llx)\n", pc, base,
             pc > base ? pc - base : 0);
    port_log("    sp   %016llx\n", sp);
    port_log("    atos -o <binary> -l 0x%llx 0x%llx\n", base, pc);
    {
        Dl_info di;
        if (pc && dladdr((void*)(uintptr_t)pc, &di) && di.dli_sname) {
            port_log("    in   %s  (%s)\n", di.dli_sname,
                     di.dli_fname ? di.dli_fname : "?");
        }
    }
#else
    (void)uap;
    port_log("\n*** port: signal %d at address %p\n", sig, info ? info->si_addr : NULL);
#endif
    port_stub_report();
    _exit(128 + sig);
}

/* --watchdog N: a hang is the most likely M1 failure, and a hung game gives a
 * debugger nothing to unwind because it runs on the port's own stack.  SIGALRM
 * lands in the same handler, which prints the program counter to look up with
 * atos and the stub table, whose last entry is nearly always next to the
 * problem. */
void port_watchdog_arm(int seconds) { alarm((unsigned)seconds); }

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
