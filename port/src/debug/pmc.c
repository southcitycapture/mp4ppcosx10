/* M43 (PLAN.md 58.4): the G4's performance counters on the game thread.
 *
 * `--pmc N` programs the 7455's six counters with event set N (below) for
 * the game thread and splits what they count by region: the engine's walks
 * (Hu3DExec's own loops, the shadow pass, the motion system, the object
 * walk of Hu3DDraw, Hu3DDrawPost, Hu3DModelObjMtxGet, the processes of
 * HuPrcCall), the port's GX front end, the audio tick and the rest -- each
 * region exclusive of the ones nested in it, and separately for drawn and
 * consumed frames.  The report is printed at exit.
 *
 * No CHUD: Leopard's PowerPC kernel answers the PPC special syscall 0x600B
 * (xnu-1228 osfmk/ppc/hw_perfmon.c, perfmon_control) for any thread; the
 * counters are virtual per thread and count user mode only (the kernel
 * saves and zeroes them on every exception).  The region reads are the
 * user-readable UPMC SPRs (a few cycles each; checked against the
 * syscall's 64-bit read by tests/pmc_test.c on the G4), the low 31 bits,
 * with the kernel's overflow reset (to zero at 2^31) folded in.
 *
 * The engine's regions are entered from wrappers the measurement build
 * alone has (PMC_WRAP=1: patches-pmc.txt renames the game's bodies to
 * *_game and src/debug/pmc_wrap.c calls them between an enter and a
 * leave); the shipped build has the port's own regions only. */
#include "port.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#if defined(__APPLE__) && (defined(__ppc__) || defined(__powerpc__))
#define PMC_HAVE 1
#include <mach/mach.h>
#endif

static const char* const region_name[PMC_R_N] = {
    "rest", "HuPrcCall (processes)", "Hu3DExec (its own loops)", "Hu3DShadowExec",
    "Hu3DMotionExec", "Hu3DDraw (object walk)", "Hu3DDrawPost", "Hu3DModelObjMtxGet",
    "port GX", "audio tick", "present",
    /* M44: the GX region's sub-regions (port.h PERF_SUB_*) */
    "GX decode/job", "GX cpu-xf", "GX state (xf+raster)", "GX texbind", "GX issue",
    "GX index", "GX tev", "GX vprog draw", "GX vprog bind", "GX attr order",
    "GX vcache keys", "GX prim/batch", "GX job build", "GX pending last", "GX job record",
    "GX batch flush",
};

/* the events per set, PMC1..PMC6 (MPC7450UM chapter 11) */
static const int pmc_sets[3][6] = {
    { 1, 37, 23, 2, 6, 7 },
    { 1, 39, 21, 23, 19, 30 },
    { 1, 38, 18, 2, 20, 29 },
};
static const char* const pmc_set_names[3][6] = {
    { "cycles", "L1D load miss", "L1D miss", "instr", "L2 data miss", "L3 data miss" },
    { "cycles", "L1D store miss", "LMQ wait cyc", "DTLB search cyc", "L2 miss", "L3 miss" },
    { "cycles", "dcbt L1 miss", "DTLB miss", "instr", "L3 miss", "L2 miss" },
};

int pmc_on;
static int pmc_set;
static unsigned pmc_port;
static pthread_t pmc_thread; /* the game thread: a region entered elsewhere is not counted */
static unsigned last[6];
static int stack[32];
static int depth;
/* [drawn][region][counter], and the calls into each region */
static unsigned long long acc[2][PMC_R_N][6];
static unsigned long calls[2][PMC_R_N];
static unsigned long frames[2];
/* the frame in progress: whether it is drawn is known only at its end */
static unsigned long long fr_acc[PMC_R_N][6];
static unsigned long fr_calls[PMC_R_N];
/* --pmcwin A,B: count only frames A..B from the minigame's entry (a run with
 * --minigame), or retrace frames A..B (anything else); 0,0 = every frame */
static int fr_active;
unsigned long port_mg_entry(void);
unsigned long VIGetRetraceCount(void);
static int pmc_window_open(void) {
    unsigned long base = 0, f;
    if (!port_opt.pmcwin_to) {
        return 1;
    }
    if (port_opt.minigame) {
        base = port_mg_entry();
        if (!base) {
            return 0;
        }
    }
    f = VIGetRetraceCount() - base;
    return (int)f >= port_opt.pmcwin_from && (int)f < port_opt.pmcwin_to;
}

#ifdef PMC_HAVE
#define PM_CLEAR 0x2
#define PM_START 0x4
#define PM_STOP 0x8
#define PM_READ 0x10
#define PM_ENABLE 0x10000
#define PM_DISABLE 0x20000
#define PM_SET_EVENT 0x30000

static int pmc_call(unsigned port, int action, int pmc, int val, unsigned long long* buf) {
    register unsigned r3 __asm__("r3") = port;
    register int r4 __asm__("r4") = action;
    register int r5 __asm__("r5") = pmc;
    register int r6 __asm__("r6") = val;
    register unsigned long long* r7 __asm__("r7") = buf;
    register int r0 __asm__("r0") = 0x600B;
    __asm__ volatile("sc\n\tnop"
                     : "+r"(r3), "+r"(r4), "+r"(r5), "+r"(r6), "+r"(r7), "+r"(r0)
                     :
                     : "r8", "r9", "r10", "r11", "r12", "ctr", "lr", "cr0", "cr1", "cr5",
                       "cr6", "cr7", "xer", "memory");
    return (int)r3;
}

static inline void read6(unsigned* v) {
    __asm__ volatile("mfspr %0,937" : "=r"(v[0]));
    __asm__ volatile("mfspr %0,938" : "=r"(v[1]));
    __asm__ volatile("mfspr %0,941" : "=r"(v[2]));
    __asm__ volatile("mfspr %0,942" : "=r"(v[3]));
    __asm__ volatile("mfspr %0,929" : "=r"(v[4]));
    __asm__ volatile("mfspr %0,930" : "=r"(v[5]));
}
#else
static inline void read6(unsigned* v) { memset(v, 0, 6 * sizeof(*v)); }
#endif

/* charge what the counters moved since the last read to the region on top */
static inline void charge(void) {
    unsigned v[6];
    int i, r = depth ? stack[depth - 1] : PMC_R_REST;
    read6(v);
    for (i = 0; i < 6; i++) {
        unsigned d = (v[i] - last[i]) & 0x7fffffffu; /* the kernel resets to 0 at 2^31 */
        fr_acc[r][i] += d;
        last[i] = v[i];
    }
}

void port_pmc_init(void) {
#ifdef PMC_HAVE
    int i, r;
    if (!port_opt.pmc) {
        return;
    }
    pmc_set = port_opt.pmc >= 1 && port_opt.pmc <= 3 ? port_opt.pmc - 1 : 0;
    pmc_port = mach_thread_self();
    r = pmc_call(pmc_port, PM_ENABLE, 0, 0, NULL);
    if (r) {
        port_log("port> pmc: the kernel's perfmon_control refused (%d); --pmc is off\n", r);
        return;
    }
    for (i = 0; i < 6; i++) {
        pmc_call(pmc_port, PM_SET_EVENT, i, pmc_sets[pmc_set][i], NULL);
    }
    pmc_call(pmc_port, PM_CLEAR | PM_START, 0, 0, NULL);
    read6(last);
    fr_active = pmc_window_open();
    pmc_thread = pthread_self();
    pmc_on = 1;
    port_sub_on = 1; /* M44: the GX sub-regions too */
    port_log("port> pmc: set %d on the game thread (%s, %s, %s, %s, %s, %s)\n", pmc_set + 1,
             pmc_set_names[pmc_set][0], pmc_set_names[pmc_set][1], pmc_set_names[pmc_set][2],
             pmc_set_names[pmc_set][3], pmc_set_names[pmc_set][4], pmc_set_names[pmc_set][5]);
#endif
}

void port_pmc_enter(int region) {
    if (!pmc_on || !pthread_equal(pthread_self(), pmc_thread)) {
        return;
    }
    charge();
    if (depth < 32) {
        stack[depth] = region;
    }
    depth++;
    fr_calls[region]++;
}

void port_pmc_leave(void) {
    if (!pmc_on || !pthread_equal(pthread_self(), pmc_thread)) {
        return;
    }
    charge();
    if (depth > 0) {
        depth--;
    }
}

/* the retrace gate: the frame that just ended was drawn or consumed */
void port_pmc_frame(int drawn) {
    int r, i, d = drawn ? 1 : 0;
    if (!pmc_on || !pthread_equal(pthread_self(), pmc_thread)) {
        return;
    }
    charge();
    if (fr_active) {
        frames[d]++;
        for (r = 0; r < PMC_R_N; r++) {
            for (i = 0; i < 6; i++) {
                acc[d][r][i] += fr_acc[r][i];
            }
            calls[d][r] += fr_calls[r];
        }
    }
    memset(fr_acc, 0, sizeof(fr_acc));
    memset(fr_calls, 0, sizeof(fr_calls));
    fr_active = pmc_window_open();
}

void port_pmc_report(void) {
    int d, r, i;
    if (!pmc_on) {
        return;
    }
    charge();
    for (d = 1; d >= 0; d--) {
        unsigned long long tot[6];
        unsigned long n = frames[d] ? frames[d] : 1;
        memset(tot, 0, sizeof(tot));
        for (r = 0; r < PMC_R_N; r++) {
            for (i = 0; i < 6; i++) {
                tot[i] += acc[d][r][i];
            }
        }
        port_log("port> pmc: %s frames %lu, set %d, per frame (user mode, the game thread):\n",
                 d ? "drawn" : "consumed", frames[d], pmc_set + 1);
        port_log("port> pmc:   %-26s %8s %9s", "region", "calls/fr", "Mcycles");
        for (i = 1; i < 6; i++) {
            port_log(" %15s", pmc_set_names[pmc_set][i]);
        }
        port_log("\n");
        for (r = 0; r <= PMC_R_N; r++) {
            const unsigned long long* a = r < PMC_R_N ? acc[d][r] : tot;
            if (r < PMC_R_N && a[0] == 0) {
                continue;
            }
            port_log("port> pmc:   %-26s %8.1f %9.3f", r < PMC_R_N ? region_name[r] : "(total)",
                     r < PMC_R_N ? (double)calls[d][r] / (double)n : 0.0, (double)a[0] / n / 1e6);
            for (i = 1; i < 6; i++) {
                port_log(" %15.0f", (double)a[i] / (double)n);
            }
            port_log("\n");
        }
    }
#ifdef PMC_HAVE
    pmc_call(pmc_port, PM_STOP, 0, 0, NULL);
    pmc_call(pmc_port, PM_DISABLE, 0, 0, NULL);
    mach_port_deallocate(mach_task_self(), pmc_port);
#endif
    pmc_on = 0;
}
