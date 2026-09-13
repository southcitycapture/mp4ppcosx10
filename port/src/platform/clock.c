/* One clock for both targets.
 *
 * `clock_gettime` only arrived in Mac OS X 10.12, and the G4 build targets
 * 10.4; `mach_absolute_time` has been there since NeXT and is the same call on
 * arm64.  Everything in the port that needs wall time goes through here.
 */
#include "port.h"

#include <mach/mach_time.h>

unsigned long long port_now_ns(void) {
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) {
        mach_timebase_info(&tb);
    }
    return mach_absolute_time() * tb.numer / tb.denom;
}

double port_now_seconds(void) { return (double)port_now_ns() * 1e-9; }
