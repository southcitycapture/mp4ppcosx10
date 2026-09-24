/* M43 (PLAN.md 58.4): the G4's performance counters from user space, no CHUD.
 * Leopard's PowerPC kernel (xnu-1228 osfmk/ppc/hw_perfmon.c) answers the
 * PPC special syscall 0x600B, perfmon_control: per thread, user mode only.
 * This test enables it, programs six events, runs three walks of known
 * working set (16 KB: L1; 192 KB: L2; 8 MB: memory) and prints the counts
 * read through the syscall and, beside them, through the user-readable
 * UPMC SPRs -- so the fast read the port uses is checked against the
 * kernel's 64-bit one.
 *   pmc_test [set]      set 1 (default), 2 or 3 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach.h>

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
                     : "r8", "r9", "r10", "r11", "r12", "ctr", "lr", "cr0", "cr1", "cr5", "cr6", "cr7", "xer", "memory");
    return (int)r3;
}

static inline unsigned upmc(int i) {
    unsigned v = 0;
    switch (i) {
        case 0: __asm__ volatile("mfspr %0,937" : "=r"(v)); break;
        case 1: __asm__ volatile("mfspr %0,938" : "=r"(v)); break;
        case 2: __asm__ volatile("mfspr %0,941" : "=r"(v)); break;
        case 3: __asm__ volatile("mfspr %0,942" : "=r"(v)); break;
        case 4: __asm__ volatile("mfspr %0,929" : "=r"(v)); break;
        case 5: __asm__ volatile("mfspr %0,930" : "=r"(v)); break;
    }
    return v;
}

static const int sets[3][6] = {
    { 1, 37, 23, 2, 6, 7 },   /* cycles, L1D load miss, L1D miss, instr, L2 data miss, L3 data miss */
    { 1, 39, 21, 23, 19, 30 },/* cycles, L1D store miss, LMQ wait cycles, DTLB search cycles, L2 total, L3 total */
    { 1, 38, 18, 2, 20, 29 }, /* cycles, dcbt L1 miss, DTLB miss, instr, L3 total, L2 total */
};

static volatile unsigned sink;

int main(int argc, char** argv) {
    int set = argc > 1 ? atoi(argv[1]) - 1 : 0;
    unsigned port = mach_thread_self();
    unsigned long long b0[8], b1[8];
    unsigned u0[6], u1[6];
    int sizes[3] = { 16 << 10, 192 << 10, 8 << 20 };
    int k, i, r;
    if (set < 0 || set > 2) set = 0;
    r = pmc_call(port, PM_ENABLE, 0, 0, NULL);
    printf("enable: %d\n", r);
    if (r) return 1;
    for (i = 0; i < 6; i++) {
        r = pmc_call(port, PM_SET_EVENT, i, sets[set][i], NULL);
        if (r) printf("set event pmc%d=%d: %d\n", i + 1, sets[set][i], r);
    }
    r = pmc_call(port, PM_CLEAR | PM_START, 0, 0, NULL);
    printf("clear|start: %d\n", r);
    for (k = 0; k < 3; k++) {
        int n = sizes[k];
        unsigned* a = (unsigned*)malloc(n);
        int pass, j;
        unsigned s = 0;
        memset(a, 1, n);
        for (pass = 0; pass < 2; pass++) { for (j = 0; j < n / 4; j += 8) s += a[j]; } /* warm */
        memset(b0, 0, sizeof(b0));
        pmc_call(port, PM_READ, 0, 0, b0);
        for (i = 0; i < 6; i++) u0[i] = upmc(i);
        for (pass = 0; pass < 20; pass++) {
            for (j = 0; j < n / 4; j += 8) s += a[j]; /* one load per 32-byte line */
        }
        for (i = 0; i < 6; i++) u1[i] = upmc(i);
        pmc_call(port, PM_READ, 0, 0, b1);
        sink = s;
        printf("walk %7d bytes x20 (%d lines):", n, 20 * n / 32);
        for (i = 0; i < 6; i++) {
            printf("  pmc%d[%d] %llu/%u", i + 1, sets[set][i], b1[i] - b0[i], (u1[i] - u0[i]) & 0x7fffffffu);
        }
        printf("\n");
        free(a);
    }
    r = pmc_call(port, PM_STOP, 0, 0, NULL);
    r = pmc_call(port, PM_DISABLE, 0, 0, NULL);
    printf("disable: %d\n", r);
    mach_port_deallocate(mach_task_self(), port);
    return 0;
}
