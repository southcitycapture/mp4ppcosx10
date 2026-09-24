/* The matrix library, checked against itself.
 *
 *   make -C port TARGET=host mtxtest && port/build-host/mtx_test
 *
 * `-DMTX_USE_C` redirects every `MTX*` macro at the SDK's C bodies in
 * `src/dolphin/mtx/`, and `port/src/os/psmtx_c.c` supplies the `PS*` names the
 * game calls directly, because the paired-single originals are Metrowerks
 * assembly the mirror drops.  Two things can go wrong in that swap and neither
 * shows up as a compile error:
 *
 *   1. **A constructor that does not write all twelve elements.**  On the
 *      console `MTXIdentity` is `PSMTXIdentity`, which stores six paired
 *      singles and therefore always zeroes the translation column.  The C body
 *      it is swapped for may write only the 3x3, and then a caller that hands
 *      it an uninitialised stack `Mtx` gets whatever was there -- which is
 *      exactly the M4/M5 off-world pass (see PLAN.md 15.1).  The test poisons
 *      the destination and demands every element be written.
 *
 *   2. **Aliasing.**  `PSMTXConcat` allows `dst == src`; so do the SDK's C
 *      bodies, through a temporary.  A hand-written replacement that forgot
 *      that would corrupt `MTXConcat(m, x, m)`, which `mtxRotCat` does three
 *      times per call.  The test runs every in-place form against the
 *      out-of-place answer.
 *
 *   3. **A C body that computes the wrong thing entirely.**  Nothing on the
 *      console ever called these, so nothing ever checked them:
 *      `C_VECScale` was `C_VECNormalize`'s body under the wrong name, which is
 *      `1/sqrtf(0)` on a zero vector and therefore NaN (PLAN.md 15.2).  Each
 *      one is checked against the arithmetic written out by hand, and fed the
 *      zero vector.
 *
 * Plus the two functions with no C original at all -- `PSMTXReorder` and
 * `PSMTXROMultVecArray` -- against `C_MTXMultVecArray`, which is the identity
 * the pair is supposed to preserve.
 *
 * Exit status is the number of failures.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include <dolphin/types.h>
#include <dolphin/mtx.h>

/* PS* names the port defines but the header only declares under GEKKO. */
void PSMTXIdentity(Mtx m);
void PSMTXCopy(const Mtx src, Mtx dst);
void PSMTXConcat(const Mtx a, const Mtx b, Mtx ab);
u32 PSMTXInverse(const Mtx src, Mtx inv);
u32 PSMTXInvXpose(const Mtx src, Mtx invX);
void PSMTXRotTrig(Mtx m, char axis, f32 sinA, f32 cosA);
void PSMTXRotAxisRad(Mtx m, const Vec* axis, f32 rad);
void PSMTXTrans(Mtx m, f32 x, f32 y, f32 z);
void PSMTXScale(Mtx m, f32 x, f32 y, f32 z);
void PSMTXMultVec(const Mtx m, const Vec* src, Vec* dst);
void PSMTXMultVecArray(const Mtx m, const Vec* srcBase, Vec* dstBase, u32 count);
void PSMTXReorder(const Mtx src, ROMtx dest);
void PSMTXROMultVecArray(const ROMtx m, const Vec* srcBase, Vec* dstBase, u32 count);
void PSVECNormalize(const Vec* src, Vec* dst);

static int failures;
static unsigned rng_state = 0x1234567u;

static float frand(void) {
    rng_state = rng_state * 1103515245u + 12345u;
    return (float)((int)((rng_state >> 8) & 0xFFFF) - 32768) / 4096.0f;
}

/* A bit pattern no sane matrix element carries, so "was this written?" is a
 * question with an answer. */
#define POISON 0x7F8ABCDEu

static void poison(Mtx m) {
    int i;
    u32* p = (u32*)m;
    for (i = 0; i < 12; i++) {
        p[i] = POISON;
    }
}

static void fill(Mtx m) {
    int r, c;
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 4; c++) {
            m[r][c] = frand();
        }
    }
}

static void fail(const char* what, const char* detail) {
    printf("  FAIL  %-28s %s\n", what, detail);
    failures++;
}

/* ---- 1. every constructor writes all twelve elements ---------------------- */

static void check_written(const char* name, const Mtx m) {
    const u32* p = (const u32*)m;
    char buf[128];
    int i, n = 0;
    for (i = 0; i < 12; i++) {
        if (p[i] == POISON) {
            n++;
        }
    }
    if (n) {
        snprintf(buf, sizeof(buf),
                 "%d of 12 elements left unwritten (r0 %d%d%d%d r1 %d%d%d%d r2 %d%d%d%d)", n,
                 p[0] == POISON, p[1] == POISON, p[2] == POISON, p[3] == POISON,
                 p[4] == POISON, p[5] == POISON, p[6] == POISON, p[7] == POISON,
                 p[8] == POISON, p[9] == POISON, p[10] == POISON, p[11] == POISON);
        fail(name, buf);
    }
}

static void test_constructors(void) {
    Mtx m, src;
    Vec axis = { 0.3f, 0.5f, 0.81f };
    Quaternion q = { 0.2f, 0.3f, 0.4f, 0.84f };
    Vec p = { 1.0f, 2.0f, 3.0f }, n = { 0.0f, 1.0f, 0.0f };
    Vec eye = { 10.0f, 20.0f, 30.0f }, up = { 0.0f, 1.0f, 0.0f },
        at = { 0.0f, 0.0f, 0.0f };

    printf("constructors write the whole 3x4:\n");
    poison(m); C_MTXIdentity(m);                     check_written("C_MTXIdentity", m);
    poison(m); PSMTXIdentity(m);                     check_written("PSMTXIdentity", m);
    poison(m); C_MTXScale(m, 2, 3, 4);               check_written("C_MTXScale", m);
    poison(m); C_MTXTrans(m, 2, 3, 4);               check_written("C_MTXTrans", m);
    poison(m); C_MTXRotRad(m, 'X', 0.5f);            check_written("C_MTXRotRad X", m);
    poison(m); C_MTXRotRad(m, 'Y', 0.5f);            check_written("C_MTXRotRad Y", m);
    poison(m); C_MTXRotRad(m, 'Z', 0.5f);            check_written("C_MTXRotRad Z", m);
    poison(m); C_MTXRotAxisRad(m, &axis, 0.7f);      check_written("C_MTXRotAxisRad", m);
    poison(m); C_MTXQuat(m, &q);                     check_written("C_MTXQuat", m);
    poison(m); C_MTXReflect(m, &p, &n);              check_written("C_MTXReflect", m);
    poison(m); C_MTXLookAt(m, &eye, &up, &at);       check_written("C_MTXLookAt", m);
    fill(src);
    poison(m); C_MTXTranspose(src, m);               check_written("C_MTXTranspose", m);
    poison(m); C_MTXConcat(src, src, m);             check_written("C_MTXConcat", m);
    C_MTXIdentity(src); src[0][3] = 3.0f; src[1][3] = -2.0f; src[2][3] = 1.0f;
    poison(m); C_MTXInverse(src, m);                 check_written("C_MTXInverse", m);
    poison(m); C_MTXInvXpose(src, m);                check_written("C_MTXInvXpose", m);
}

/* ---- 2. the in-place forms match the out-of-place answer ------------------ */

static int same(const Mtx a, const Mtx b, float tol) {
    int r, c;
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 4; c++) {
            float d = a[r][c] - b[r][c];
            if (d > tol || d < -tol) {
                return 0;
            }
        }
    }
    return 1;
}

static void test_aliasing(void) {
    int trial;
    printf("aliasing (dst == src) matches the out-of-place answer:\n");
    for (trial = 0; trial < 2000; trial++) {
        Mtx a, b, want, got;
        fill(a);
        fill(b);

        C_MTXConcat(a, b, want);
        C_MTXCopy(a, got);
        C_MTXConcat(got, b, got);
        if (!same(want, got, 1e-4f)) {
            fail("C_MTXConcat(a,b,a)", "differs from C_MTXConcat(a,b,c)");
            break;
        }
        C_MTXCopy(b, got);
        C_MTXConcat(a, got, got);
        if (!same(want, got, 1e-4f)) {
            fail("C_MTXConcat(a,b,b)", "differs from C_MTXConcat(a,b,c)");
            break;
        }
        PSMTXConcat(a, b, got);
        if (!same(want, got, 1e-4f)) {
            fail("PSMTXConcat", "differs from C_MTXConcat");
            break;
        }

        C_MTXTranspose(a, want);
        C_MTXCopy(a, got);
        C_MTXTranspose(got, got);
        if (!same(want, got, 1e-4f)) {
            fail("C_MTXTranspose(a,a)", "differs from the out-of-place answer");
            break;
        }

        /* An arbitrary random 3x4 need not be invertible; use a rotation with
         * a translation, which always is. */
        C_MTXRotRad(a, 'Y', frand());
        a[0][3] = frand(); a[1][3] = frand(); a[2][3] = frand();
        C_MTXInverse(a, want);
        C_MTXCopy(a, got);
        C_MTXInverse(got, got);
        if (!same(want, got, 1e-4f)) {
            fail("C_MTXInverse(a,a)", "differs from the out-of-place answer");
            break;
        }
        C_MTXInvXpose(a, want);
        C_MTXCopy(a, got);
        C_MTXInvXpose(got, got);
        if (!same(want, got, 1e-4f)) {
            fail("C_MTXInvXpose(a,a)", "differs from the out-of-place answer");
            break;
        }
    }
}

/* ---- 3. the PS* forwarders agree with the C_* bodies ---------------------- */

static void test_forwarders(void) {
    int trial;
    printf("PS* forwarders agree with the C_* bodies:\n");
    for (trial = 0; trial < 2000; trial++) {
        Mtx want, got, src;
        Vec axis;
        Vec v, wv, gv;
        float a = frand();
        fill(src);
        axis.x = frand(); axis.y = frand(); axis.z = frand() + 3.0f;
        v.x = frand(); v.y = frand(); v.z = frand();

        C_MTXTrans(want, 1.5f, -2.5f, 3.5f);
        PSMTXTrans(got, 1.5f, -2.5f, 3.5f);
        if (!same(want, got, 0.0f)) { fail("PSMTXTrans", "differs"); break; }

        C_MTXScale(want, 1.5f, -2.5f, 3.5f);
        PSMTXScale(got, 1.5f, -2.5f, 3.5f);
        if (!same(want, got, 0.0f)) { fail("PSMTXScale", "differs"); break; }

        C_MTXRotTrig(want, 'Z', sinf(a), cosf(a));
        PSMTXRotTrig(got, 'Z', sinf(a), cosf(a));
        if (!same(want, got, 0.0f)) { fail("PSMTXRotTrig", "differs"); break; }

        C_MTXRotAxisRad(want, &axis, a);
        PSMTXRotAxisRad(got, &axis, a);
        if (!same(want, got, 0.0f)) { fail("PSMTXRotAxisRad", "differs"); break; }

        C_MTXIdentity(want);
        PSMTXIdentity(got);
        if (!same(want, got, 0.0f)) { fail("PSMTXIdentity", "differs from C_MTXIdentity"); break; }

        C_MTXMultVec(src, &v, &wv);
        PSMTXMultVec(src, &v, &gv);
        if (fabsf(wv.x - gv.x) + fabsf(wv.y - gv.y) + fabsf(wv.z - gv.z) > 1e-4f) {
            fail("PSMTXMultVec", "differs"); break;
        }
        C_VECNormalize(&axis, &wv);
        PSVECNormalize(&axis, &gv);
        if (fabsf(wv.x - gv.x) + fabsf(wv.y - gv.y) + fabsf(wv.z - gv.z) > 1e-5f) {
            fail("PSVECNormalize", "differs"); break;
        }
    }
}

/* ---- 4. the reordered pair round-trips ------------------------------------ */
/* `PSMTXReorder` + `PSMTXROMultVecArray` exist only as paired-single assembly,
 * so there is nothing to diff them against -- but together they are supposed
 * to be `MTXMultVecArray`, and that *is* in C.  The SDK's own loop reads two
 * vectors at a time and its counter maths needs count >= 3. */

static void test_reorder(void) {
    int trial;
    printf("PSMTXReorder + PSMTXROMultVecArray == C_MTXMultVecArray:\n");
    for (trial = 0; trial < 500; trial++) {
        Mtx m;
        ROMtx ro;
        Vec src[16], want[16], got[16];
        u32 count = 3 + (u32)(trial % 13);
        u32 i;
        fill(m);
        for (i = 0; i < count; i++) {
            src[i].x = frand();
            src[i].y = frand();
            src[i].z = frand();
        }
        C_MTXMultVecArray(m, src, want, count);
        PSMTXReorder(m, ro);
        PSMTXROMultVecArray(ro, src, got, count);
        for (i = 0; i < count; i++) {
            if (fabsf(want[i].x - got[i].x) + fabsf(want[i].y - got[i].y) +
                    fabsf(want[i].z - got[i].z) >
                1e-3f) {
                char buf[128];
                snprintf(buf, sizeof(buf), "vector %u of %u: want %.3f %.3f %.3f, got %.3f %.3f %.3f",
                         i, count, want[i].x, want[i].y, want[i].z, got[i].x, got[i].y,
                         got[i].z);
                fail("PSMTXReorder/ROMultVecArray", buf);
                return;
            }
        }
        /* in place, which EnvelopeExec.c does not do but m417Dll's water does */
        memcpy(got, src, sizeof(Vec) * count);
        PSMTXROMultVecArray(ro, got, got, count);
        for (i = 0; i < count; i++) {
            if (fabsf(want[i].x - got[i].x) > 1e-3f) {
                fail("PSMTXROMultVecArray(src==dst)", "differs from the out-of-place answer");
                return;
            }
        }
    }
}

/* ---- 4b. the AltiVec PSMTXROMultVecArray against the scalar body, bit for
 * bit (M17, PLAN.md 32).  A million random vertices through both, including
 * every alignment a 12-byte stride visits and the in-place form; a single
 * differing bit is a failure, because the port's reference frames are md5s
 * and the skinning writes every character's vertices through this loop. */
extern int port_mtx_noaltivec;

static void test_romult_altivec(void) {
    int trial, ndiff = 0, ntested = 0;
    unsigned worst = 0;
    printf("PSMTXROMultVecArray AltiVec == scalar, bit for bit:\n");
    for (trial = 0; trial < 20000; trial++) {
        Mtx m;
        ROMtx ro;
        Vec src[64], a[64], b[64];
        u32 count = 1 + (u32)(trial % 53);
        u32 i;
        fill(m);
        if (trial % 7 == 0) {
            /* large and tiny magnitudes, negative zeros, the rounding edges */
            m[0][3] = frand() * 1e6f;
            m[1][1] = -0.0f;
            m[2][2] = frand() * 1e-6f;
        }
        for (i = 0; i < count; i++) {
            src[i].x = frand() * (trial & 1 ? 1000.0f : 1.0f);
            src[i].y = frand() * (trial & 2 ? 1e-3f : 1.0f);
            src[i].z = (trial % 11 == 0) ? -0.0f : frand();
        }
        PSMTXReorder(m, ro);
        port_mtx_noaltivec = 1;
        PSMTXROMultVecArray(ro, src, a, count);
        port_mtx_noaltivec = 0;
        PSMTXROMultVecArray(ro, src, b, count);
        for (i = 0; i < count; i++) {
            unsigned d = (memcmp(&a[i].x, &b[i].x, 4) != 0) + (memcmp(&a[i].y, &b[i].y, 4) != 0) +
                         (memcmp(&a[i].z, &b[i].z, 4) != 0);
            ntested++;
            if (d) {
                if (ndiff < 5) {
                    char buf[160];
                    snprintf(buf, sizeof(buf),
                             "vector %u of %u: scalar %.9g %.9g %.9g  altivec %.9g %.9g %.9g", i,
                             count, a[i].x, a[i].y, a[i].z, b[i].x, b[i].y, b[i].z);
                    fail("PSMTXROMultVecArray(altivec)", buf);
                }
                ndiff++;
                if (d > worst) {
                    worst = d;
                }
            }
        }
        /* in place, both ways */
        memcpy(a, src, sizeof(Vec) * count);
        port_mtx_noaltivec = 1;
        PSMTXROMultVecArray(ro, a, a, count);
        memcpy(b, src, sizeof(Vec) * count);
        port_mtx_noaltivec = 0;
        PSMTXROMultVecArray(ro, b, b, count);
        if (memcmp(a, b, sizeof(Vec) * count) != 0) {
            fail("PSMTXROMultVecArray(altivec, src==dst)", "differs from the scalar in-place answer");
            ndiff++;
        }
    }
    port_mtx_noaltivec = 0;
    printf("  %d vertices, %d differ\n", ntested, ndiff);
}

/* ---- 5. the VEC family against arithmetic written out by hand ------------ */
/* The `PS*` vector functions the game calls are paired-single assembly the
 * mirror drops, so what actually runs is either a `C_VEC*` body or a port
 * forwarder -- and a `C_VEC*` body the console never called can be quietly
 * wrong.  `C_VECScale` was: it ignored `scale` and normalised, which is
 * `1/sqrtf(0)` on a zero vector and therefore NaN (PLAN.md 15.2).  So each one
 * is checked against the arithmetic spelled out here, and every one is also
 * fed the zero vector, because that is the input that turns a wrong body from
 * a wrong answer into a NaN that spreads. */

static int close3(const Vec* got, float x, float y, float z, float tol) {
    return fabsf(got->x - x) <= tol && fabsf(got->y - y) <= tol &&
           fabsf(got->z - z) <= tol;
}

static void test_vec(void) {
    int trial;
    Vec zero = { 0.0f, 0.0f, 0.0f };
    Vec r;
    printf("the VEC family computes what it says, and the zero vector is finite:\n");
    for (trial = 0; trial < 2000; trial++) {
        Vec a, b;
        float s = frand();
        a.x = frand(); a.y = frand(); a.z = frand();
        b.x = frand(); b.y = frand(); b.z = frand();

        C_VECScale(&a, &r, s);
        if (!close3(&r, a.x * s, a.y * s, a.z * s, 1e-4f)) {
            fail("C_VECScale", "does not multiply by the scale");
            break;
        }
        C_VECAdd(&a, &b, &r);
        if (!close3(&r, a.x + b.x, a.y + b.y, a.z + b.z, 1e-5f)) {
            fail("C_VECAdd", "differs"); break;
        }
        C_VECSubtract(&a, &b, &r);
        if (!close3(&r, a.x - b.x, a.y - b.y, a.z - b.z, 1e-5f)) {
            fail("C_VECSubtract", "differs"); break;
        }
        C_VECCrossProduct(&a, &b, &r);
        if (!close3(&r, a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                    a.x * b.y - a.y * b.x, 1e-4f)) {
            fail("C_VECCrossProduct", "differs"); break;
        }
        if (fabsf(C_VECDotProduct(&a, &b) - (a.x * b.x + a.y * b.y + a.z * b.z)) > 1e-4f) {
            fail("C_VECDotProduct", "differs"); break;
        }
        if (fabsf(C_VECMag(&a) - sqrtf(a.x * a.x + a.y * a.y + a.z * a.z)) > 1e-4f) {
            fail("C_VECMag", "differs"); break;
        }
    }
    /* the zero vector, everywhere it can reach */
    C_VECScale(&zero, &r, 0.15f);
    if (r.x != r.x || r.y != r.y || r.z != r.z) {
        fail("C_VECScale(0)", "produced NaN");
    }
    C_VECNormalize(&zero, &r);
    if (r.x != r.x || r.y != r.y || r.z != r.z) {
        fail("C_VECNormalize(0)", "produced NaN");
    }
    C_VECHalfAngle(&zero, &zero, &r);
    if (r.x != r.x || r.y != r.y || r.z != r.z) {
        fail("C_VECHalfAngle(0,0)", "produced NaN");
    }
    C_VECReflect(&zero, &zero, &r);
    if (r.x != r.x || r.y != r.y || r.z != r.z) {
        fail("C_VECReflect(0,0)", "produced NaN");
    }
    if (C_VECMag(&zero) != 0.0f) {
        fail("C_VECMag(0)", "is not zero");
    }
}

/* ---- 6. M28 (c): the sparse concats of the bone walk against C_MTXConcat,
 * bit for bit (PLAN.md 43).  Random matrices whose elements are, one in
 * four, a zero of either sign -- the case the sign rule exists for -- and
 * angles that include 0 and the axes' multiples; both the out-of-place and
 * the in-place (ab == a, which SetEnvelopMtx uses) forms. */
#include "port.h"
PortOptions port_opt;
void port_log(const char* fmt, ...) { (void)fmt; }
void port_mtx_concat_trans(const Mtx a, f32 x, f32 y, f32 z, Mtx ab);
void port_mtx_concat_rot(const Mtx a, char axis, f32 rad, Mtx ab);
void port_sincosf(f32 rad, f32* s, f32* c);
f32 port_sqrtf(f32 x);

static float frand_z(void) {
    unsigned r;
    rng_state = rng_state * 1103515245u + 12345u;
    r = (rng_state >> 8) & 0xFF;
    if (r < 32) {
        return 0.0f;
    }
    if (r < 64) {
        return -0.0f;
    }
    if (r < 72) {
        return frand() * 1e-20f; /* tiny: the underflowing products */
    }
    return frand();
}

static int bits_differ(const Mtx a, const Mtx b) {
    return memcmp(a, b, sizeof(Mtx)) != 0;
}

static void test_sparse_concat(void) {
    int trial, ndiff = 0;
    static const char axes[3] = {'z', 'y', 'x'};
    printf("sparse concats == C_MTXConcat, bit for bit:\n");
    port_opt.nosparsemtx = 0;
    for (trial = 0; trial < 400000; trial++) {
        Mtx a, want, got, r, t;
        int i, j, ax;
        f32 x = frand_z(), y = frand_z(), z = frand_z(), rad;
        for (i = 0; i < 3; i++) {
            for (j = 0; j < 4; j++) {
                a[i][j] = frand_z();
            }
        }
        /* translation */
        PSMTXTrans(t, x, y, z);
        C_MTXConcat(a, t, want);
        port_mtx_concat_trans(a, x, y, z, got);
        if (bits_differ(want, got)) {
            ndiff++;
        }
        PSMTXCopy(a, got);
        port_mtx_concat_trans(got, x, y, z, got);
        if (bits_differ(want, got)) {
            ndiff++;
        }
        /* rotations */
        ax = trial % 3;
        switch (trial % 7) {
        case 0: rad = 0.0f; break;
        case 1: rad = -0.0f; break;
        case 2: rad = 3.14159265f; break;
        case 3: rad = 1.5707963f; break;
        default: rad = frand() * 0.05f; break;
        }
        {
            f32 sn, cs;
            port_sincosf(rad, &sn, &cs);
            PSMTXRotTrig(r, axes[ax], sn, cs);
        }
        C_MTXConcat(a, r, want);
        port_mtx_concat_rot(a, axes[ax], rad, got);
        if (bits_differ(want, got)) {
            ndiff++;
            if (ndiff <= 4) {
                printf("  axis %c rad %.9g: row0 want %.9g %.9g %.9g %.9g got %.9g %.9g %.9g %.9g\n",
                       axes[ax], rad, want[0][0], want[0][1], want[0][2], want[0][3],
                       got[0][0], got[0][1], got[0][2], got[0][3]);
            }
        }
        PSMTXCopy(a, got);
        port_mtx_concat_rot(got, axes[ax], rad, got);
        if (bits_differ(want, got)) {
            ndiff++;
        }
    }
    printf("  %d of 1,600,000 concats differ\n", ndiff);
    if (ndiff) {
        fail("sparse concat", "differs from C_MTXConcat");
    }
}

/* ---- 6b. M41: the port's register-blocked C_MTXConcat against the SDK's
 * body (C_MTXConcat_sdk), bit for bit, out of place and in both aliasing
 * forms (ab == a, ab == b), over matrices with signed zeros, tiny and huge
 * elements (the underflowing and overflowing products); then the speed of
 * each over two million calls (PLAN.md 56). */
void C_MTXConcat_sdk(const Mtx a, const Mtx b, Mtx ab);
static float frand_zb(void) {
    unsigned r;
    rng_state = rng_state * 1103515245u + 12345u;
    r = (rng_state >> 8) & 0xFF;
    if (r < 8) {
        return frand() * 1e20f; /* huge: the overflowing products */
    }
    return frand_z();
}
static void test_blocked_concat(void) {
    int trial, ndiff = 0;
    Mtx a, b, want, got;
    printf("the blocked C_MTXConcat == the SDK's, bit for bit:\n");
    port_opt.nofastconcat = 0;
    for (trial = 0; trial < 500000; trial++) {
        int i, j;
        for (i = 0; i < 3; i++) {
            for (j = 0; j < 4; j++) {
                a[i][j] = frand_zb();
                b[i][j] = frand_zb();
            }
        }
        C_MTXConcat_sdk(a, b, want);
        C_MTXConcat(a, b, got);
        ndiff += bits_differ(want, got);
        PSMTXCopy(a, got);
        C_MTXConcat(got, b, got);
        ndiff += bits_differ(want, got);
        PSMTXCopy(b, got);
        C_MTXConcat(a, got, got);
        ndiff += bits_differ(want, got);
        PSMTXCopy(a, got);
        C_MTXConcat_sdk(a, a, want);
        C_MTXConcat(got, got, got);
        ndiff += bits_differ(want, got);
    }
    printf("  %d of 2,000,000 concats differ\n", ndiff);
    if (ndiff) {
        fail("blocked C_MTXConcat", "differs from C_MTXConcat_sdk");
    }
    {
        clock_t t0;
        double s_sdk, s_blk;
        int k;
        for (k = 0; k < 3; k++) {
            int j2;
            for (j2 = 0; j2 < 4; j2++) {
                a[k][j2] = frand();
                b[k][j2] = frand();
            }
        }
        t0 = clock();
        for (k = 0; k < 2000000; k++) {
            C_MTXConcat_sdk(a, b, a);
            a[0][3] = b[0][0]; /* keep it finite and live */
        }
        s_sdk = (double)(clock() - t0) / CLOCKS_PER_SEC;
        t0 = clock();
        for (k = 0; k < 2000000; k++) {
            C_MTXConcat(a, b, a);
            a[0][3] = b[0][0];
        }
        s_blk = (double)(clock() - t0) / CLOCKS_PER_SEC;
        printf("  2M in-place concats: the SDK's %.0f ms, blocked %.0f ms\n", s_sdk * 1000.0,
               s_blk * 1000.0);
    }
}

/* ---- 7. M28 (d): port_sqrtf against libm's sqrtf.  Default: sixteen million
 * random floats over the whole range plus the edges; `--sqrt-all` every
 * positive normal float, all 2^31 - 2^23 of them (minutes on the G4), which
 * is the proof.  Then the speed of each over ten million calls. */
static void test_sqrt(int all) {
    unsigned long n = 0, ndiff = 0;
    union { f32 f; u32 u; } k;
    printf("port_sqrtf == sqrtf, bit for bit (%s):\n",
           all > 1 ? "every positive normal float, strided" : all ? "every positive normal float" : "16M random");
    port_opt.nofastsqrt = 0;
    if (all) {
        for (k.u = 0x00800000u; k.u < 0x7F800000u; k.u += (u32)all) {
            union { f32 f; u32 u; } a, b;
            a.f = port_sqrtf(k.f);
            b.f = sqrtf(k.f);
            n++;
            if (a.u != b.u) {
                ndiff++;
                if (ndiff <= 8) {
                    printf("  x=%.9g (0x%08x): port %.9g (0x%08x) libm %.9g (0x%08x)\n", k.f, k.u,
                           a.f, a.u, b.f, b.u);
                }
            }
        }
    } else {
        unsigned long i;
        for (i = 0; i < 16000000UL; i++) {
            union { f32 f; u32 u; } a, b;
            rng_state = rng_state * 1103515245u + 12345u;
            k.u = (rng_state << 8) ^ (rng_state >> 5);
            k.u = 0x00800000u + (k.u % (0x7F800000u - 0x00800000u));
            a.f = port_sqrtf(k.f);
            b.f = sqrtf(k.f);
            n++;
            if (a.u != b.u) {
                ndiff++;
                if (ndiff <= 8) {
                    printf("  x=%.9g (0x%08x): port %.9g (0x%08x) libm %.9g (0x%08x)\n", k.f, k.u,
                           a.f, a.u, b.f, b.u);
                }
            }
        }
    }
    {
        /* the edges: zero, -0, denormals, FLT_MAX, inf, NaN, negative */
        static const u32 edges[] = {0u, 0x80000000u, 1u, 0x007FFFFFu, 0x00800000u, 0x7F7FFFFFu,
                                    0x7F800000u, 0x7FC00000u, 0xBF800000u, 0x3F800000u, 0x40800000u};
        unsigned e;
        for (e = 0; e < sizeof(edges) / sizeof(edges[0]); e++) {
            union { f32 f; u32 u; } a, b;
            k.u = edges[e];
            a.f = port_sqrtf(k.f);
            b.f = sqrtf(k.f);
            n++;
            if (a.u != b.u && !(a.f != a.f && b.f != b.f)) {
                ndiff++;
                printf("  edge x=0x%08x: port 0x%08x libm 0x%08x\n", k.u, a.u, b.u);
            }
        }
    }
    printf("  %lu of %lu differ\n", ndiff, n);
    if (ndiff) {
        fail("port_sqrtf", "differs from libm's sqrtf");
    }
    {
        /* speed: the same ten million inputs through each, a dependency chain
         * so the latency is what is timed (VECNormalize's 1/sqrt is one) */
        unsigned long i;
        float acc = 0.0f;
        clock_t t0, t1, t2;
        port_opt.nofastsqrt = 0;
        t0 = clock();
        for (i = 0; i < 10000000UL; i++) {
            acc = port_sqrtf(acc + 1.5f + (float)(i & 1023));
        }
        t1 = clock();
        for (i = 0; i < 10000000UL; i++) {
            acc = sqrtf(acc + 1.5f + (float)(i & 1023));
        }
        t2 = clock();
        printf("  10M chained: port_sqrtf %.0f ms, libm sqrtf %.0f ms (acc %g)\n",
               (t1 - t0) * 1000.0 / CLOCKS_PER_SEC, (t2 - t1) * 1000.0 / CLOCKS_PER_SEC, acc);
    }
}

static void bench_concat(void) {
    /* a bone walk's shape: T then Rz Ry Rx, 2M bones, each form */
    Mtx a, r, t, out;
    unsigned long i;
    clock_t t0, t1, t2;
    int j, k;
    for (j = 0; j < 3; j++) {
        for (k = 0; k < 4; k++) {
            a[j][k] = frand();
        }
    }
    port_opt.nosparsemtx = 0;
    t0 = clock();
    for (i = 0; i < 2000000UL; i++) {
        f32 ang = (float)(i & 255) * 0.01f;
        port_mtx_concat_trans(a, 1.0f, 2.0f, 3.0f, out);
        port_mtx_concat_rot(out, 'z', ang, out);
        port_mtx_concat_rot(out, 'y', ang, out);
        port_mtx_concat_rot(out, 'x', ang, out);
        a[0][3] = out[0][3] * 1e-9f;
    }
    t1 = clock();
    for (i = 0; i < 2000000UL; i++) {
        f32 ang = (float)(i & 255) * 0.01f;
        PSMTXTrans(t, 1.0f, 2.0f, 3.0f);
        PSMTXConcat(a, t, out);
        PSMTXRotRad(r, 'z', ang);
        PSMTXConcat(out, r, out);
        PSMTXRotRad(r, 'y', ang);
        PSMTXConcat(out, r, out);
        PSMTXRotRad(r, 'x', ang);
        PSMTXConcat(out, r, out);
        a[0][3] = out[0][3] * 1e-9f;
    }
    t2 = clock();
    printf("  2M bones (T Rz Ry Rx): sparse %.0f ms, general %.0f ms\n",
           (t1 - t0) * 1000.0 / CLOCKS_PER_SEC, (t2 - t1) * 1000.0 / CLOCKS_PER_SEC);
}

int main(int argc, char** argv) {
    /* --sqrt-all [STRIDE]: every positive normal float (2^31 - 2^23 of them),
     * or every STRIDE-th (the stride prime to the mantissa's pattern) */
    int all = argc > 1 && !strcmp(argv[1], "--sqrt-all") ? (argc > 2 ? atoi(argv[2]) : 1) : 0;
    printf("---- port/tests/mtx_test ----\n");
    test_constructors();
    test_aliasing();
    test_forwarders();
    test_reorder();
    test_romult_altivec();
    test_vec();
    test_sparse_concat();
    test_blocked_concat();
    bench_concat();
    test_sqrt(all);
    printf("---- %d failure(s) ----\n", failures);
    return failures ? 1 : 0;
}
