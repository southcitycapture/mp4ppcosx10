/* The two pieces of arithmetic the vertex pipeline does per vertex, done the
 * way a 7450 wants them done.
 *
 * M3 profiled the title screen on the real G4 with `sample` and found the
 * frame was not going where §12.7 had guessed.  Per-draw GL state was 98.9%
 * eliminated by the shadow in gl13.c and the frame barely moved, because the
 * cost was never the driver: of 4,300 in-thread samples, 1,727 were in
 * `transform_and_store`, 995 in `read_component`/`indexed`, 448 in
 * `gx_tex_bind` -- and **768 were in `sqrt`**, reached through a dyld stub.
 * The GL entry points together came to about forty.
 *
 * Two things cause that, and both are PowerPC-specific:
 *
 *  - **`sqrtf` is a function call into double-precision `sqrt`.**  The 74xx
 *    does not implement the optional `fsqrt`/`fsqrts` instructions at all, so
 *    the 10.4 libm cannot inline them; every normal normalisation and every
 *    light-distance calculation left the port through a stub.  What the 74xx
 *    *does* have is `frsqrte`, a five-bit reciprocal-square-root estimate,
 *    which two Newton-Raphson steps refine to about twenty bits -- far more
 *    than a byte-quantised vertex colour or an eight-bit normal can show.
 *    And a reciprocal square root is what both callers actually want: they
 *    divide by the length, so the divides go with it.
 *  - **floating-point division is not pipelined.**  `fdivs` is 14 to 21
 *    cycles with nothing else issuing behind it, and the indexed-attribute
 *    reader was doing one per component to apply the vertex-attribute table's
 *    fractional shift -- up to eight per vertex.  The shift is a power of two,
 *    so the reciprocal is exact and a table of them costs nothing in
 *    precision.
 *
 * Everything here stays deterministic: it is fixed arithmetic, not an
 * estimate that varies with the machine, so `--seed` runs still compare
 * byte-for-byte with each other.  They do not compare with a build from
 * before this file, and should not be expected to.
 */
#ifndef PORT_GX_MATH_H
#define PORT_GX_MATH_H

#include <math.h>

/* 1 / (1 << n), exactly, for the vertex-attribute table's fractional shift.
 * GX allows 0..31; the game never uses more than 16. */
extern const float gx_frac_scale[32];

/* 1 / sqrt(x), with x > 0 guaranteed by the caller checking for zero. */
#if (defined(__ppc__) || defined(__POWERPC__) || defined(__powerpc__)) && !defined(__ppc64__)
static __inline__ float gx_rsqrtf(float x) {
    double e;
    float y;
    __asm__("frsqrte %0,%1" : "=f"(e) : "f"((double)x));
    y = (float)e;
    y = y * (1.5f - 0.5f * x * y * y);
    y = y * (1.5f - 0.5f * x * y * y);
    return y;
}
#else
static __inline__ float gx_rsqrtf(float x) { return 1.0f / sqrtf(x); }
#endif

#endif
