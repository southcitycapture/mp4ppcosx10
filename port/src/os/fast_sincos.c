/* M42 (PLAN.md 57.3): libm's sinf/cosf, answered without libm where that is
 * provably the same float.
 *
 * Leopard's sinf and cosf are wrappers: `bl _sin; frsp f1,f1` -- the double
 * routine, rounded to float (otool -tV /usr/lib/libSystem.B.dylib on the G4).
 * Each double call is ~250 ns on the 7450 and the sin/cos memo's misses pay
 * two of them (m441's consumed frame: 3% in libm's sin/cos, M42's profile).
 *
 * Here: the argument reduced by pi/2 in three exact-product steps (the split
 * constants are the usual 33/33/53-bit pieces of pi/2), sine and cosine of the
 * reduced argument by their Taylor series to degree 15 and 16 (truncation
 * under 2^-55 relative on |y| <= pi/4), in double.  The result is within a few
 * units of the double's last place of the true value, as libm's is.  Both
 * round to the same float unless the true value lies within those few units
 * of a point halfway between two floats -- so the double is rounded only
 * when its low 29 bits (the bits a float drops) are more than GUARD units
 * away from that halfway point; otherwise the caller asks libm.  The guard
 * turns away about one call in 2^21.
 *
 * The domain is 2^-26 <= |x| <= 1024 (radians) and the two zeros; everything
 * else goes to libm.  For |x| <= 1024 the reduction's first product n*P1
 * has at most 43 bits and x - n*P1 at most 42 on P1's grid, both exact.
 * port/tests/sincos_test.c compares every float of the domain -- 603,979,778
 * of them, both signs, and the zeros -- against libm's sinf and cosf on the
 * G4 itself: PLAN.md 57.3 has the count of differences (0).  --nofastsin:
 * libm always. */
#include <math.h>

#include "port.h"

#define GUARD 64u

typedef union {
    double d;
    unsigned long long u;
} DBits;

static const double PIO2_1 = 1.57079632673412561417e+00;  /* 0x3FF921FB54400000 */
static const double PIO2_2 = 6.07710050630396597660e-11;  /* 0x3DD0B4611A600000 */
static const double PIO2_3 = 2.02226624871116645580e-21;  /* 0x3BA3198A2E000000 */
static const double PIO2_3T = 8.47842766036889956997e-32; /* 0x397B839A252049C1 */
static const double INVPIO2 = 6.36619772367581382433e-01;

/* 1: d rounds to float the same way anything within GUARD ulps of it does */
static inline int safe(double d) {
    DBits b;
    unsigned lo;
    b.d = d;
    lo = (unsigned)(b.u & 0x1FFFFFFFull);
    return lo > 0x10000000u + GUARD || lo < 0x10000000u - GUARD;
}

static inline double ksin(double y, double z) {
    double p = 1.0 / 355687428096000.0;
    p = p * z - 1.0 / 1307674368000.0;
    p = p * z + 1.0 / 6227020800.0;
    p = p * z - 1.0 / 39916800.0;
    p = p * z + 1.0 / 362880.0;
    p = p * z - 1.0 / 5040.0;
    p = p * z + 1.0 / 120.0;
    p = p * z - 1.0 / 6.0;
    return y + y * z * p;
}

static inline double kcos(double z) {
    double p = -1.0 / 6402373705728000.0;
    p = p * z + 1.0 / 20922789888000.0;
    p = p * z - 1.0 / 87178291200.0;
    p = p * z + 1.0 / 479001600.0;
    p = p * z - 1.0 / 3628800.0;
    p = p * z + 1.0 / 40320.0;
    p = p * z - 1.0 / 720.0;
    p = p * z + 1.0 / 24.0;
    p = p * z - 0.5;
    return 1.0 + z * p;
}

static unsigned long fs_fast, fs_libm;

/* 1 and *s, *c set: the floats libm's sinf/cosf return for x; 0: ask libm */
int port_fast_sincosf(float x, float* s, float* c) {
    double xd = x, ax = fabs(xd), y0, y1, z, sd, cd, rs, rc;
    int n;
    if (xd == 0.0) {
        *s = x; /* sin(+-0) = +-0, cos(+-0) = 1: libm's own answers */
        *c = 1.0f;
        fs_fast++;
        return 1;
    }
    if (!(ax >= 1.490116119384765625e-08 && ax <= 1024.0)) { /* 2^-26; also NaN */
        fs_libm++;
        return 0;
    }
    if (ax <= 0.78539816339744830962) {
        n = 0;
        y0 = xd;
        y1 = 0.0;
    } else {
        double fn, r, p, sm, bb, e, e2;
        n = (int)(xd * INVPIO2 + (xd < 0 ? -0.5 : 0.5));
        fn = (double)n;
        /* x - n*pi/2 as a double-double: x - n*P1 is exact (x is a float
         * of |x| <= 1024, n*P1 has 43 bits on P1's grid), n*P2 and n*P3 are
         * exact products, the sum with P2's piece is an exact TwoSum, and
         * what is left (n*P3, n*P3T) is far below the result's last place */
        r = xd - fn * PIO2_1;
        p = -(fn * PIO2_2);
        sm = r + p;
        bb = sm - r;
        e = (r - (sm - bb)) + (p - bb);
        e2 = e - fn * PIO2_3 - fn * PIO2_3T;
        y0 = sm + e2;
        y1 = (sm - y0) + e2;
    }
    z = y0 * y0;
    sd = ksin(y0, z) + y1 * (1.0 - 0.5 * z);
    cd = kcos(z) - y1 * y0;
    switch (n & 3) {
    case 0: rs = sd; rc = cd; break;
    case 1: rs = cd; rc = -sd; break;
    case 2: rs = -sd; rc = -cd; break;
    default: rs = -cd; rc = sd; break;
    }
    if (!safe(rs) || !safe(rc)) {
        fs_libm++;
        return 0;
    }
    *s = (float)rs;
    *c = (float)rc;
    fs_fast++;
    return 1;
}

/* the pair as libm answers it, the fast way when it can */
void port_sincosf_libm(float x, float* s, float* c) {
    if (port_opt.nofastsin || !port_fast_sincosf(x, s, c)) {
        *s = sinf(x);
        *c = cosf(x);
    }
}

void port_fast_sincos_report(void) {
    if (fs_fast + fs_libm) {
        port_log("port> fast sin/cos (M42): %lu pairs without libm, %lu through libm%s\n", fs_fast, fs_libm,
                 port_opt.nofastsin ? " (--nofastsin)" : "");
    }
}
