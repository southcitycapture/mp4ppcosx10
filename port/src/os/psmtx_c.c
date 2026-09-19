/* The paired-single matrix library, in scalar C.
 *
 * The game calls `PSMTX*` / `PSVEC*` directly at 219 sites, and `-DMTX_USE_C`
 * only redirects the generic `MTX*` / `VEC*` macros -- it does not remove the
 * `PS*` bodies from the SDK sources, which are Metrowerks paired-single
 * assembly and are dropped by the port's mirror.  Everything the game actually
 * calls is defined here: mostly forwarders to the `C_MTX*` bodies that are
 * already in the tree, plus the handful of functions the SDK never wrote in C.
 *
 * Two of those have no C equivalent in any SDK decompilation and are written
 * from the register-transfer semantics of `psmtx.c`:
 *
 *   PSMTXReorder        (19 sites) transposes a 3x4 row-major Mtx into the
 *                       4x3 "reordered" ROMtx the skinning fast paths want:
 *                       three rows of translation-free 3-vectors followed by
 *                       the translation column.
 *   PSMTXROMultVecArray (18 sites) is MTXMultVecArray against that layout.
 *
 * The 7450 has no paired singles at all, so there is nothing to preserve here
 * but the arithmetic.  This file is the first place to reach for AltiVec if
 * --perf ever says so: MTX/VEC is 74 symbols over 4,172 call sites, the
 * hottest SDK family after GX.
 */
#include <math.h>

#include <dolphin/types.h>
#include <dolphin/mtx.h>

/* ---- the C_ bodies the SDK sources never spelled out --------------------- */

void C_MTXMultVec(const Mtx m, const Vec* src, Vec* dst) {
    f32 x = src->x, y = src->y, z = src->z;
    f32 ox = m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3];
    f32 oy = m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3];
    f32 oz = m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3];
    dst->x = ox;
    dst->y = oy;
    dst->z = oz;
}

void C_MTXMultVecArray(const Mtx m, const Vec* srcBase, Vec* dstBase, u32 count) {
    u32 i;
    for (i = 0; i < count; i++) {
        C_MTXMultVec(m, &srcBase[i], &dstBase[i]);
    }
}

void C_MTXMultVecSR(const Mtx m, const Vec* src, Vec* dst) {
    f32 x = src->x, y = src->y, z = src->z;
    f32 ox = m[0][0] * x + m[0][1] * y + m[0][2] * z;
    f32 oy = m[1][0] * x + m[1][1] * y + m[1][2] * z;
    f32 oz = m[2][0] * x + m[2][1] * y + m[2][2] * z;
    dst->x = ox;
    dst->y = oy;
    dst->z = oz;
}

void C_VECAdd(const Vec* a, const Vec* b, Vec* r) {
    r->x = a->x + b->x;
    r->y = a->y + b->y;
    r->z = a->z + b->z;
}

void C_VECSubtract(const Vec* a, const Vec* b, Vec* r) {
    r->x = a->x - b->x;
    r->y = a->y - b->y;
    r->z = a->z - b->z;
}

f32 C_VECDotProduct(const Vec* a, const Vec* b) {
    return a->x * b->x + a->y * b->y + a->z * b->z;
}

void C_VECCrossProduct(const Vec* a, const Vec* b, Vec* r) {
    f32 x = a->y * b->z - a->z * b->y;
    f32 y = a->z * b->x - a->x * b->z;
    f32 z = a->x * b->y - a->y * b->x;
    r->x = x;
    r->y = y;
    r->z = z;
}

f32 C_VECSquareMag(const Vec* v) { return v->x * v->x + v->y * v->y + v->z * v->z; }
f32 C_VECMag(const Vec* v) { return sqrtf(C_VECSquareMag(v)); }

void C_VECNormalize(const Vec* src, Vec* dst) {
    f32 m = C_VECSquareMag(src);
    if (m > 0.0f) {
        m = 1.0f / sqrtf(m);
    }
    dst->x = src->x * m;
    dst->y = src->y * m;
    dst->z = src->z * m;
}

f32 C_VECSquareDistance(const Vec* a, const Vec* b) {
    f32 dx = a->x - b->x, dy = a->y - b->y, dz = a->z - b->z;
    return dx * dx + dy * dy + dz * dz;
}

f32 C_VECDistance(const Vec* a, const Vec* b) { return sqrtf(C_VECSquareDistance(a, b)); }

/* ---- PS* forwarders ------------------------------------------------------ */

/* PSMTXIdentity is six paired-single stores and therefore writes all twelve
 * elements, translation column included.  C_MTXIdentity used to write only the
 * 3x3 -- patches.txt fixes that in the mirror, and port/tests/mtx_test.c holds
 * it fixed, because a caller with an uninitialised stack Mtx cannot tell. */
void PSMTXIdentity(Mtx m) { C_MTXIdentity(m); }
void PSMTXCopy(const Mtx src, Mtx dst) { C_MTXCopy(src, dst); }
void PSMTXConcat(const Mtx a, const Mtx b, Mtx ab) { C_MTXConcat(a, b, ab); }
u32 PSMTXInverse(const Mtx src, Mtx inv) { return C_MTXInverse(src, inv); }
u32 PSMTXInvXpose(const Mtx src, Mtx invX) { return C_MTXInvXpose(src, invX); }
void PSMTXRotTrig(Mtx m, char axis, f32 sinA, f32 cosA) { C_MTXRotTrig(m, axis, sinA, cosA); }
void PSMTXRotAxisRad(Mtx m, const Vec* axis, f32 rad) { C_MTXRotAxisRad(m, axis, rad); }
void PSMTXTrans(Mtx m, f32 x, f32 y, f32 z) { C_MTXTrans(m, x, y, z); }
void PSMTXScale(Mtx m, f32 x, f32 y, f32 z) { C_MTXScale(m, x, y, z); }
void PSMTXMultVec(const Mtx m, const Vec* src, Vec* dst) { C_MTXMultVec(m, src, dst); }
void PSMTXMultVecArray(const Mtx m, const Vec* srcBase, Vec* dstBase, u32 count) {
    C_MTXMultVecArray(m, srcBase, dstBase, count);
}
void PSVECNormalize(const Vec* src, Vec* dst) { C_VECNormalize(src, dst); }

/* ---- the two with no C original ------------------------------------------ */

/* Mtx is row major, 3 rows of 4: [ R | t ].  ROMtx is 4 rows of 3: the three
 * *columns* of R, then t.  So dest[j][i] = src[i][j] for the rotation part and
 * dest[3][i] = src[i][3] for the translation. */
void PSMTXReorder(const Mtx src, ROMtx dest) {
    int i, j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            dest[j][i] = src[i][j];
        }
        dest[3][i] = src[i][3];
    }
}

/* The AltiVec form (M17, PLAN.md 32).  The board's consumed frame spends
 * 9.5% of itself in this loop (the CPU skinning's vertex pass), and it is
 * a vector op by construction: one output vertex is three dot products of
 * the same (x, y, z, 1) against the four ROMtx rows, which is exactly one
 * lane each.
 *
 * Bit-exact with the scalar body, deliberately: GCC contracts
 * `m0*x + m1*y + m2*z + m3` as fmuls(m1, y), fmadds(m0, x, .),
 * fmadds(m2, z, .), fadds(m3, .) (the middle product first, then the two
 * fused terms, then the plain add -- read off build-ppc-darwin/psmtx.s),
 * and vmaddfp is a fused multiply-add with the same single rounding, so the
 * same four operations in the same order give the same bits.  The first
 * product is a vmaddfp with -0.0 as the addend, which is the multiply's
 * rounded value with its sign intact.  port/tests/mtx_test.c checks the two
 * bodies agree bit for bit on 540,000 random vertices (0 differ, on the G4).
 * `--altivec` runs it; see port_mtx_noaltivec below for why it is not the
 * default. */
#ifdef __ALTIVEC__
#include <altivec.h>
#undef vector
#undef pixel
#undef bool

static void romult_altivec(const ROMtx m, const Vec* srcBase, Vec* dstBase, u32 count) {
    const __vector float nzero = (__vector float){ -0.0f, -0.0f, -0.0f, -0.0f };
    const u8* mp = (const u8*)m;
    __vector float M0, M1, M2, M3;
    u32 i;
    /* the four rows, unaligned: lvx reads the two aligned blocks either side
     * and vperm picks the sixteen bytes that start at the row.  The fourth
     * lane of each is whatever follows and is never stored. */
#define LDROW(off) ((__vector float)vec_perm(vec_ld(off, mp), vec_ld((off) + 15, mp), \
                                             vec_lvsl(off, mp)))
    M0 = LDROW(0);
    M1 = LDROW(12);
    M2 = LDROW(24);
    M3 = LDROW(36);
#undef LDROW
    for (i = 0; i < count; i++) {
        const u8* s = (const u8*)&srcBase[i];
        u8* d = (u8*)&dstBase[i];
        __vector float v = (__vector float)vec_perm(vec_ld(0, s), vec_ld(15, s), vec_lvsl(0, s));
        __vector float xs = vec_splat(v, 0);
        __vector float ys = vec_splat(v, 1);
        __vector float zs = vec_splat(v, 2);
        __vector float t = vec_madd(M1, ys, nzero);
        __vector float r;
        t = vec_madd(M0, xs, t);
        t = vec_madd(M2, zs, t);
        r = vec_add(M3, t);
        /* three element stores; the rotate puts element j in the lane that
         * (d + 4j) selects */
        r = vec_perm(r, r, vec_lvsr(0, d));
        vec_ste(r, 0, (float*)d);
        vec_ste(r, 4, (float*)d);
        vec_ste(r, 8, (float*)d);
    }
}
#endif

/* Off unless --altivec: measured on the G4 (PLAN.md 32), the consumed board
 * frame is 10.46 ms with the scalar loop and 10.82 with this one.  The
 * unaligned loads, the permutes and the three element stores per vertex
 * cost more than the FPU pipeline GCC builds for the scalar body -- the same
 * verdict M5 reached for the vertex path (PLAN.md 15.6).  Kept because it is
 * exact and the test proves it, and because the next AltiVec attempt should
 * start from a measured baseline rather than from a belief. */
int port_mtx_noaltivec = 1;

void PSMTXROMultVecArray(const ROMtx m, const Vec* srcBase, Vec* dstBase, u32 count) {
    u32 i;
#ifdef __ALTIVEC__
    if (!port_mtx_noaltivec) {
        romult_altivec(m, srcBase, dstBase, count);
        return;
    }
#endif
    for (i = 0; i < count; i++) {
        f32 x = srcBase[i].x, y = srcBase[i].y, z = srcBase[i].z;
        dstBase[i].x = m[0][0] * x + m[1][0] * y + m[2][0] * z + m[3][0];
        dstBase[i].y = m[0][1] * x + m[1][1] * y + m[2][1] * z + m[3][1];
        dstBase[i].z = m[0][2] * x + m[1][2] * y + m[2][2] * z + m[3][2];
    }
}
