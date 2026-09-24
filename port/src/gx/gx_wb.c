/* M42 (PLAN.md 57.5): a write barrier for the vertex cache's arrays.
 *
 * The static-geometry cache (M40, gx_draw.c) keys every primitive on the
 * bytes of the arrays it reads, and because the game may write an array at
 * any moment, every array is re-hashed at its first use in each drawn frame
 * (gx_vc_epoch).  On a scene of static geometry that is megabytes a frame
 * of hashing that always finds the same bytes: m432's maze, 9.4% of the
 * game thread in `vc_hash` alone (M42's profile).
 *
 * The barrier answers "has anything written these bytes since I hashed
 * them?" with the MMU instead of a hash.  When an array is hashed, the pages
 * lying WHOLLY inside it are made read-only first (never a partial page: a
 * page shared with anything else -- a heap header, a coroutine stack, an
 * object the game writes every frame -- is never protected); a write to one
 * of them faults, the handler (crash.c calls port_wb_fault first) makes the
 * page writable again and remembers that it was written, and the store
 * retries.  The next check of an array needs a full hash only if one of its
 * interior pages was written (or re-armed later by another array's hash, or
 * never armed); otherwise it hashes the partial pages at its two ends alone,
 * and if those match the array is unchanged -- as the full hash would have
 * said, without reading the rest.
 *
 * What cannot fault: the kernel writing user memory (read/pread/fread into
 * MEM1 return EFAULT on a read-only page) and a device's DMA.  The port's
 * three kernel writers into MEM1 -- the disc reads (dvd_fs.c, dvd_cache.c)
 * and the snapshot restore -- call port_wb_disarm on their destination
 * first; a free (port_mem_freed) disarms the block, so a buffer reused for
 * a new load starts unwatched.  No device writes into MEM1 (the EFB copies
 * are glCopyTexSubImage2D into GL textures or glReadPixels, a user-space
 * copy).  A page that faults a second time is hot (a buffer the game
 * rewrites every frame) and is never protected again: those arrays are
 * hashed every frame as before, with no faults.
 *
 * Only MEM1 is watched.  --nowb: every array hashed at every epoch, as M40
 * and M41 did. */
#include <dolphin/types.h>
#include "port.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define WB_PAGE 4096u
#define WB_SHIFT 12

enum { WB_OFF = 0, WB_ARMED = 1, WB_HOT = 2 };

static volatile u8* wb_state;     /* per page of MEM1 */
static volatile u8* wb_faults;    /* faults seen, saturating */
static volatile u32* wb_arm_ser;  /* the serial the page was last armed at */
static u32 wb_serial = 1;
static uintptr_t wb_lo, wb_hi;
static unsigned long wb_npages;
static int wb_on = -1;
static volatile unsigned long st_faults, st_hot;
static unsigned long st_arms, st_arm_calls, st_clean, st_dirty, st_disarm;
static double st_bytes_skipped;

static int wb_init(void) {
    if (wb_on >= 0) {
        return wb_on;
    }
    wb_on = 0;
    if (port_opt.nowb || getenv("PORT_NO_CRASH_HANDLER")) {
        return 0;
    }
    wb_lo = ((uintptr_t)port_mem1_lo() + WB_PAGE - 1) & ~(uintptr_t)(WB_PAGE - 1);
    wb_hi = (uintptr_t)port_mem1_hi() & ~(uintptr_t)(WB_PAGE - 1);
    if (wb_hi <= wb_lo) {
        return 0;
    }
    wb_npages = (wb_hi - wb_lo) >> WB_SHIFT;
    wb_state = (volatile u8*)calloc(wb_npages, 1);
    wb_faults = (volatile u8*)calloc(wb_npages, 1);
    wb_arm_ser = (volatile u32*)calloc(wb_npages, sizeof(u32));
    if (!wb_state || !wb_faults || !wb_arm_ser) {
        return 0;
    }
    wb_on = 1;
    return 1;
}

/* from the SIGBUS/SIGSEGV handler: 1 when the fault was a watched page's
 * first write since it was armed (the page is writable again: retry) */
int port_wb_fault(const void* addr) {
    uintptr_t a = (uintptr_t)addr;
    unsigned long p;
    if (wb_on != 1 || a < wb_lo || a >= wb_hi) {
        return 0;
    }
    p = (a - wb_lo) >> WB_SHIFT;
    if (wb_state[p] != WB_ARMED) {
        return 0; /* not ours (or already disarmed by another thread: retry anyway) */
    }
    if (mprotect((void*)(wb_lo + (p << WB_SHIFT)), WB_PAGE, PROT_READ | PROT_WRITE) != 0) {
        return 0;
    }
    if (wb_faults[p] < 255) {
        wb_faults[p]++;
    }
    wb_state[p] = wb_faults[p] >= 2 ? WB_HOT : WB_OFF;
    if (wb_state[p] == WB_HOT) {
        st_hot++;
    }
    st_faults++;
    return 1;
}

/* the whole pages inside [p, p+n) as page indexes [*a, *b); 0 when none.
 * M43 (--wbpart, PLAN.md 58.5): every page the range touches, the partial
 * ones at its ends included -- an array smaller than a page (most of an
 * object's) has no interior page and was hashed at every epoch; a page it
 * shares with something the game writes faults like any other and goes
 * hot after its second fault, exactly as an interior page does.  All of
 * the range must lie in MEM1, or none of it is armed. */
static int interior(const void* ptr, size_t n, unsigned long* a, unsigned long* b) {
    uintptr_t s = (uintptr_t)ptr, e = s + n;
    uintptr_t ps = (s + WB_PAGE - 1) & ~(uintptr_t)(WB_PAGE - 1);
    uintptr_t pe = e & ~(uintptr_t)(WB_PAGE - 1);
    if (port_opt.wbpart) {
        if (!n || s < wb_lo || e > wb_hi) {
            return 0;
        }
        ps = s & ~(uintptr_t)(WB_PAGE - 1);
        pe = (e + WB_PAGE - 1) & ~(uintptr_t)(WB_PAGE - 1);
    }
    if (ps < wb_lo) {
        ps = wb_lo;
    }
    if (pe > wb_hi) {
        pe = wb_hi;
    }
    if (pe <= ps) {
        return 0;
    }
    *a = (ps - wb_lo) >> WB_SHIFT;
    *b = (pe - wb_lo) >> WB_SHIFT;
    return 1;
}

/* before hashing [ptr, ptr+n): protect its interior pages (those not hot);
 * returns the serial to remember with the hash (0: the barrier is off) */
u32 port_wb_arm(const void* ptr, size_t n) {
    unsigned long a, b, p, run = 0;
    u32 ser;
    if (!wb_init() || !interior(ptr, n, &a, &b)) {
        return 0;
    }
    ser = ++wb_serial;
    for (p = a; p <= b; p++) {
        /* a page still armed keeps its serial: it has been protected, and so
         * unwritten, since then -- clean for every array hashed after it */
        int want = p < b && wb_state[p] == WB_OFF;
        if (want) {
            if (!run) {
                run = p + 1; /* start (+1: 0 means none) */
            }
            continue;
        }
        if (run) {
            unsigned long s = run - 1, q;
            if (mprotect((void*)(wb_lo + (s << WB_SHIFT)), (p - s) << WB_SHIFT, PROT_READ) == 0) {
                for (q = s; q < p; q++) {
                    wb_state[q] = WB_ARMED;
                    wb_arm_ser[q] = ser;
                }
                st_arms += p - s;
            }
            st_arm_calls++;
            run = 0;
        }
    }
    return ser;
}

/* 1: no interior page of [ptr, ptr+n) has been written since it was armed at
 * or before `ser` (every interior page armed; none re-armed since) */
int port_wb_clean(const void* ptr, size_t n, u32 ser) {
    unsigned long a, b, p;
    if (wb_on != 1 || !ser || !interior(ptr, n, &a, &b)) {
        return 0;
    }
    for (p = a; p < b; p++) {
        if (wb_state[p] != WB_ARMED || wb_arm_ser[p] > ser) {
            st_dirty++;
            return 0;
        }
    }
    st_clean++;
    st_bytes_skipped += (double)((b - a) << WB_SHIFT);
    return 1;
}

/* the partial pages at the two ends of [ptr, ptr+n) (everything, when the
 * range has no interior page): *h_lo, *n_lo / *h_hi, *n_hi */
void port_wb_ends(const void* ptr, size_t n, const u8** h, size_t* hn, const u8** t, size_t* tn) {
    uintptr_t s = (uintptr_t)ptr, e = s + n;
    if (port_opt.wbpart && wb_on == 1 && n && s >= wb_lo && e <= wb_hi) {
        /* M43: the ends are armed pages too: nothing is left to hash */
        *h = (const u8*)ptr; *hn = 0; *t = (const u8*)ptr + n; *tn = 0;
        return;
    }
    uintptr_t ps = (s + WB_PAGE - 1) & ~(uintptr_t)(WB_PAGE - 1);
    uintptr_t pe = e & ~(uintptr_t)(WB_PAGE - 1);
    if (ps < wb_lo) ps = wb_lo;
    if (pe > wb_hi) pe = wb_hi;
    if (pe <= ps) {
        *h = (const u8*)ptr; *hn = n; *t = (const u8*)ptr + n; *tn = 0;
        return;
    }
    *h = (const u8*)ptr; *hn = ps - s;
    *t = (const u8*)pe; *tn = e - pe;
}

/* a kernel write (or a free) is coming to [ptr, ptr+n): unprotect its pages */
void port_wb_disarm(const void* ptr, size_t n) {
    uintptr_t s = (uintptr_t)ptr & ~(uintptr_t)(WB_PAGE - 1), e = (uintptr_t)ptr + n;
    unsigned long a, b, p, run = 0;
    if (wb_on != 1 || !n || e <= wb_lo || s >= wb_hi) {
        return;
    }
    if (s < wb_lo) s = wb_lo;
    if (e > wb_hi) e = wb_hi;
    a = (s - wb_lo) >> WB_SHIFT;
    b = (e - wb_lo + WB_PAGE - 1) >> WB_SHIFT;
    for (p = a; p <= b; p++) {
        int armed = p < b && wb_state[p] == WB_ARMED;
        if (armed) {
            if (!run) run = p + 1;
            continue;
        }
        if (run) {
            unsigned long s0 = run - 1, q;
            mprotect((void*)(wb_lo + (s0 << WB_SHIFT)), (p - s0) << WB_SHIFT, PROT_READ | PROT_WRITE);
            for (q = s0; q < p; q++) {
                wb_state[q] = WB_OFF;
            }
            st_disarm += p - s0;
            run = 0;
        }
    }
}

/* HuMemMemoryFree (via port_mem_freed): the block goes back to the heap --
 * unprotected, and its whole pages forget their faults (a buffer rewritten
 * every frame and freed leaves no hot pages behind for the next owner) */
void port_wb_freed(const void* ptr, size_t n) {
    unsigned long a, b, p;
    if (wb_on != 1 || !n) {
        return;
    }
    port_wb_disarm(ptr, n);
    if (interior(ptr, n, &a, &b)) {
        for (p = a; p < b; p++) {
            if (wb_state[p] == WB_HOT) {
                wb_state[p] = WB_OFF;
            }
            wb_faults[p] = 0;
        }
    }
}

/* everything unprotected (a snapshot restore, the shutdown) */
void port_wb_disarm_all(void) {
    if (wb_on == 1) {
        port_wb_disarm((const void*)wb_lo, wb_hi - wb_lo);
    }
}

void port_wb_report(void) {
    if (wb_on == 1) {
        port_log("port> write barrier (M42%s): %lu pages armed in %lu calls, %lu faults (%lu pages hot), "
                 "%lu disarmed by kernel writes/frees; arrays clean %lu / written %lu, %.1f MB of hashing "
                 "skipped\n",
                 port_opt.wbpart ? ", --wbpart: the ends' partial pages too" : "",
                 st_arms, st_arm_calls, st_faults, st_hot, st_disarm, st_clean, st_dirty,
                 st_bytes_skipped / 1048576.0);
    } else if (port_opt.nowb) {
        port_log("port> write barrier (M42): off (--nowb)\n");
    }
}
