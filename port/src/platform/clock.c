/* One clock for both targets.
 *
 * `clock_gettime` only arrived in Mac OS X 10.12, and the G4 build targets
 * 10.4; `mach_absolute_time` has been there since NeXT and is the same call on
 * arm64.  Everything in the port that needs wall time goes through here.
 *
 * The obvious spelling of this function is wrong on PowerPC, and it is wrong
 * intermittently, which is the worst way for a clock to be wrong:
 *
 *     return mach_absolute_time() * tb.numer / tb.denom;   // NO
 *
 * On the G4 `mach_timebase_info` answers numer = 1,000,000,000 and denom = the
 * timebase frequency (about 33 MHz on a Quicksilver), so the multiply
 * overflows 64 bits after 2^64 / 1e9 = 1.84e10 timebase ticks -- **about nine
 * minutes of machine uptime**.  Past that the product wraps, and every ninth
 * minute the clock jumps backwards by a couple of centuries' worth of
 * nanoseconds.  M3 caught it as a `--perf` report with a mean frame time of
 * *minus* 485 ms; what it would have looked like in the game is a logo that
 * hangs forever or a wipe that finishes instantly, once every nine minutes,
 * on a machine that had been on for a while.  `OSGetTick` is downstream of
 * this and a dozen places in the game pace themselves off it.
 *
 * Two things fix it.  The counter is read relative to the **first** reading,
 * so the value being scaled is the length of this run rather than the
 * machine's uptime; and the scale is applied as a quotient-plus-remainder,
 * which cannot overflow for any run length that fits in the counter at all.
 * The clock therefore starts at zero, which is also what makes the
 * `--deterministic` path and this one agree about their origin.
 */
#include "port.h"

#include <mach/mach_time.h>

unsigned long long port_now_ns(void) {
    static mach_timebase_info_data_t tb;
    static unsigned long long base;
    unsigned long long t;
    if (tb.denom == 0) {
        mach_timebase_info(&tb);
        base = mach_absolute_time();
    }
    t = mach_absolute_time() - base;
    /* (t * numer) / denom without the overflow: the whole part first, then the
     * remainder, which is < denom and so cannot overflow the multiply. */
    return (t / tb.denom) * tb.numer + ((t % tb.denom) * tb.numer) / tb.denom;
}

double port_now_seconds(void) { return (double)port_now_ns() * 1e-9; }
